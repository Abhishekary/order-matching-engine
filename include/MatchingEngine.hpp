#pragma once
#include <mutex>
#include <atomic>
#include <vector>
#include <optional>
#include <functional>
#include "OrderBook.hpp"
#include "Trade.hpp"

namespace matching_engine {

// MatchingEngine owns a single-symbol OrderBook and implements price-time
// priority matching for Market and Limit orders on top of it. All public
// entry points are thread-safe: concurrent producer threads (simulating
// multiple exchange clients) may call submitOrder/cancelOrder/amendOrder
// concurrently; a single mutex serializes access to the book to guarantee
// execution consistency (no two threads can interleave matches against the
// same price level).
//
// Matching semantics:
//   * Limit order crosses the book while its limit price is at least as
//     aggressive as the best opposite price; any unfilled remainder rests
//     in the book (standard limit-order behavior).
//   * Market order sweeps the book at whatever prices are available
//     (best price improvement first) up to its full quantity; any
//     unfilled remainder is cancelled (IOC) rather than resting, since
//     market orders never rest in a matching engine.
//   * Trades always print at the RESTING order's price (price-time
//     priority + price improvement for the aggressor), consistent with
//     standard exchange matching rules.
class MatchingEngine {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    explicit MatchingEngine(TradeCallback onTrade = nullptr) : onTrade_(std::move(onTrade)) {}

    // Submits a new order. If id == 0, an id is generated internally
    // (monotonically increasing, thread-safe). Returns the id assigned to
    // the order and the trades it generated immediately (possibly empty).
    struct SubmitResult {
        OrderId id;
        std::vector<Trade> trades;
        OrderStatus finalStatus; // status of the incoming order after matching
    };

    SubmitResult submitOrder(Side side, OrderType type, Price price, Quantity qty, OrderId id = 0);

    bool cancelOrder(OrderId id);

    OrderBook::AmendResult amendOrder(OrderId id,
                                       std::optional<Price> newPrice,
                                       std::optional<Quantity> newQty);

    struct BBO {
        std::optional<Price> bid;
        std::optional<Price> ask;
    };
    BBO getBBO() const;

    std::optional<Order> getOrder(OrderId id) const;

    std::size_t totalTrades() const;
    std::vector<Trade> tradeLogSnapshot() const;

private:
    mutable std::mutex mutex_;      // guards book_ and tradeLog_
    OrderBook book_;
    std::atomic<OrderId> nextOrderId_{1};
    std::vector<Trade> tradeLog_;
    TradeCallback onTrade_;

    // Core matching loop. Caller MUST hold mutex_.
    std::vector<Trade> matchAndRest(Order incoming);
};

} // namespace matching_engine
