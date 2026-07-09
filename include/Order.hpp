#pragma once
#include <cstdint>
#include <chrono>
#include <string>

namespace matching_engine {

enum class Side : uint8_t { Buy, Sell };

enum class OrderType : uint8_t { Market, Limit };

enum class OrderStatus : uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected
};

using OrderId   = uint64_t;
using Price     = int64_t;   // fixed-point price in ticks (e.g. price * 10000) to avoid float compare issues
using Quantity  = uint64_t;
using Timestamp = uint64_t;  // nanoseconds since epoch, or a monotonic sequence counter

inline Timestamp now_ns() {
    return static_cast<Timestamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// A single resting/incoming order.
struct Order {
    OrderId    id{};
    Side       side{Side::Buy};
    OrderType  type{OrderType::Limit};
    Price      price{0};          // meaningless for Market orders
    Quantity   quantity{0};       // original quantity
    Quantity   remainingQty{0};   // quantity still open
    Timestamp  timestamp{0};      // used for price-time priority ordering
    OrderStatus status{OrderStatus::New};

    Order() = default;

    Order(OrderId id_, Side side_, OrderType type_, Price price_, Quantity qty_, Timestamp ts_)
        : id(id_), side(side_), type(type_), price(price_),
          quantity(qty_), remainingQty(qty_), timestamp(ts_), status(OrderStatus::New) {}

    bool isFilled() const { return remainingQty == 0; }
};

} // namespace matching_engine
