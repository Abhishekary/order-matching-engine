#include "MatchingEngine.hpp"

namespace matching_engine {

namespace {
inline bool limitCrosses(Side side, Price limitPrice, Price bestOppositePrice) {
    // Buy crosses if willing to pay >= best ask; Sell crosses if willing to
    // accept <= best bid.
    return side == Side::Buy ? (limitPrice >= bestOppositePrice)
                              : (limitPrice <= bestOppositePrice);
}
} // namespace

std::vector<Trade> MatchingEngine::matchAndRest(Order incoming) {
    // NOTE: caller holds mutex_.
    std::vector<Trade> trades;
    const Side oppositeSide = (incoming.side == Side::Buy) ? Side::Sell : Side::Buy;

    while (incoming.remainingQty > 0) {
        auto bestOppPriceOpt = book_.bestPrice(oppositeSide);
        if (!bestOppPriceOpt.has_value()) break; // no liquidity left on the other side

        const Price bestOppPrice = *bestOppPriceOpt;

        if (incoming.type == OrderType::Limit &&
            !limitCrosses(incoming.side, incoming.price, bestOppPrice)) {
            break; // best opposite price no longer marketable against our limit
        }

        OrderBook::PriceLevel* level = book_.bestLevel(oppositeSide);
        // level is guaranteed non-null here since bestOppPriceOpt was present.
        const Order& restingFront = level->front();

        const Quantity tradeQty = std::min(incoming.remainingQty, restingFront.remainingQty);
        const Price tradePrice = bestOppPrice; // trade prints at the resting order's price

        Trade t;
        t.price = tradePrice;
        t.quantity = tradeQty;
        t.timestamp = now_ns();
        t.aggressorSide = incoming.side;
        if (incoming.side == Side::Buy) {
            t.buyOrderId = incoming.id;
            t.sellOrderId = restingFront.id;
        } else {
            t.buyOrderId = restingFront.id;
            t.sellOrderId = incoming.id;
        }

        // Mutate the book: reduces/removes the resting order at the front
        // of the level. Must happen before we touch `restingFront` again
        // since fillFront may erase it.
        book_.fillFront(oppositeSide, bestOppPrice, tradeQty);

        incoming.remainingQty -= tradeQty;

        trades.push_back(t);
        tradeLog_.push_back(t);
        if (onTrade_) onTrade_(t);
    }

    if (incoming.remainingQty > 0) {
        if (incoming.type == OrderType::Limit) {
            incoming.status = (incoming.remainingQty == incoming.quantity)
                                   ? OrderStatus::New
                                   : OrderStatus::PartiallyFilled;
            book_.insert(incoming); // rests on the book, preserving arrival-time priority
        } else {
            // Market orders never rest: unfilled remainder is cancelled (IOC).
            incoming.status = (incoming.remainingQty == incoming.quantity)
                                   ? OrderStatus::Cancelled
                                   : OrderStatus::PartiallyFilled;
        }
    } else {
        incoming.status = OrderStatus::Filled;
    }

    return trades;
}

MatchingEngine::SubmitResult MatchingEngine::submitOrder(Side side, OrderType type, Price price,
                                                           Quantity qty, OrderId id) {
    if (id == 0) {
        id = nextOrderId_.fetch_add(1, std::memory_order_relaxed);
    }
    Order incoming(id, side, type, price, qty, now_ns());

    std::lock_guard<std::mutex> lock(mutex_);
    auto trades = matchAndRest(incoming);

    // Recover final status: if it rests, book_.find will have it; if fully
    // filled or cancelled(IOC), it won't be in the book anymore.
    OrderStatus finalStatus;
    if (const Order* resting = book_.find(id)) {
        finalStatus = resting->status;
    } else {
        finalStatus = trades.empty() ? OrderStatus::Cancelled : OrderStatus::Filled;
        // Distinguish "fully filled via trades" vs "market order with zero liquidity".
        Quantity filled = 0;
        for (const auto& t : trades) filled += t.quantity;
        if (filled > 0 && filled < qty) finalStatus = OrderStatus::PartiallyFilled;
        else if (filled == 0) finalStatus = OrderStatus::Cancelled;
        else finalStatus = OrderStatus::Filled;
    }

    return SubmitResult{id, std::move(trades), finalStatus};
}

bool MatchingEngine::cancelOrder(OrderId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return book_.cancel(id);
}

OrderBook::AmendResult MatchingEngine::amendOrder(OrderId id, std::optional<Price> newPrice,
                                                    std::optional<Quantity> newQty) {
    std::lock_guard<std::mutex> lock(mutex_);
    return book_.amend(id, newPrice, newQty, now_ns());
}

MatchingEngine::BBO MatchingEngine::getBBO() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return BBO{book_.bestBid(), book_.bestAsk()};
}

std::optional<Order> MatchingEngine::getOrder(OrderId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const Order* o = book_.find(id)) return *o;
    return std::nullopt;
}

std::size_t MatchingEngine::totalTrades() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tradeLog_.size();
}

std::vector<Trade> MatchingEngine::tradeLogSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tradeLog_;
}

} // namespace matching_engine
