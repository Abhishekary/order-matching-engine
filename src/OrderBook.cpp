#include "OrderBook.hpp"
#include <cassert>

namespace matching_engine {

OrderBook::PriceLevel& OrderBook::levelFor(Side side, Price price) {
    if (side == Side::Buy) {
        return bids_[price]; // default-constructs empty list if absent -- O(log P)
    }
    return asks_[price];
}

void OrderBook::eraseLevelIfEmpty(Side side, Price price) {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it != bids_.end() && it->second.empty()) bids_.erase(it);
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end() && it->second.empty()) asks_.erase(it);
    }
}

void OrderBook::insert(const Order& order) {
    assert(order.remainingQty > 0 && "cannot insert an order with zero remaining quantity");
    PriceLevel& level = levelFor(order.side, order.price);
    level.push_back(order); // FIFO: new orders join the back of the time-priority queue
    auto it = std::prev(level.end());
    index_[order.id] = Location{order.side, order.price, it};
}

OrderBook::PriceLevel* OrderBook::bestLevel(Side side) {
    if (side == Side::Buy) {
        if (bids_.empty()) return nullptr;
        return &bids_.begin()->second;
    }
    if (asks_.empty()) return nullptr;
    return &asks_.begin()->second;
}

std::optional<Price> OrderBook::bestPrice(Side side) const {
    if (side == Side::Buy) {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::bestBid() const { return bestPrice(Side::Buy); }
std::optional<Price> OrderBook::bestAsk() const { return bestPrice(Side::Sell); }

void OrderBook::fillFront(Side side, Price price, Quantity qty) {
    PriceLevel& level = levelFor(side, price);
    assert(!level.empty());
    Order& front = level.front();
    assert(qty <= front.remainingQty);
    front.remainingQty -= qty;
    front.status = (front.remainingQty == 0) ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
    if (front.remainingQty == 0) {
        index_.erase(front.id);
        level.pop_front(); // O(1)
        eraseLevelIfEmpty(side, price);
    }
}

bool OrderBook::cancel(OrderId id) {
    auto idxIt = index_.find(id);
    if (idxIt == index_.end()) return false;
    const Location loc = idxIt->second;
    PriceLevel& level = levelFor(loc.side, loc.price);
    level.erase(loc.it);          // O(1): std::list erase
    index_.erase(idxIt);          // O(1) average
    eraseLevelIfEmpty(loc.side, loc.price); // O(log P)
    return true;
}

OrderBook::AmendResult OrderBook::amend(OrderId id,
                                         std::optional<Price> newPrice,
                                         std::optional<Quantity> newQty,
                                         Timestamp ts) {
    auto idxIt = index_.find(id);
    if (idxIt == index_.end()) return AmendResult::NotFound;

    Location loc = idxIt->second;
    Order& resting = *loc.it;

    const Price targetPrice = newPrice.value_or(resting.price);
    const Quantity targetQty = newQty.value_or(resting.remainingQty);

    const bool priceChanged = (targetPrice != resting.price);
    const bool qtyIncreased = (targetQty > resting.remainingQty);

    if (!priceChanged && !qtyIncreased) {
        // Quantity-decrease-only (or no-op): apply in place, priority preserved.
        resting.remainingQty = targetQty;
        resting.quantity = std::max(resting.quantity, targetQty);
        resting.status = (targetQty == 0) ? OrderStatus::Cancelled : OrderStatus::PartiallyFilled;
        if (targetQty == 0) {
            // Amending down to zero is equivalent to a cancel.
            PriceLevel& level = levelFor(loc.side, loc.price);
            level.erase(loc.it);
            index_.erase(idxIt);
            eraseLevelIfEmpty(loc.side, loc.price);
        }
        return AmendResult::InPlace;
    }

    // Price change or quantity increase => cancel/replace, loses time priority.
    Order replacement = resting;
    replacement.price = targetPrice;
    replacement.quantity = targetQty;
    replacement.remainingQty = targetQty;
    replacement.timestamp = ts;
    replacement.status = OrderStatus::New;

    PriceLevel& oldLevel = levelFor(loc.side, loc.price);
    oldLevel.erase(loc.it);
    index_.erase(idxIt);
    eraseLevelIfEmpty(loc.side, loc.price);

    insert(replacement);
    return AmendResult::Requeued;
}

const Order* OrderBook::find(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return nullptr;
    return &(*it->second.it);
}

Order* OrderBook::find(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return nullptr;
    return &(*it->second.it);
}

bool OrderBook::empty(Side side) const {
    return side == Side::Buy ? bids_.empty() : asks_.empty();
}

std::size_t OrderBook::levelCount(Side side) const {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

} // namespace matching_engine
