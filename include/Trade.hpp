#pragma once
#include "Order.hpp"

namespace matching_engine {

// A single executed trade (fill) resulting from matching two orders.
struct Trade {
    OrderId   buyOrderId{};
    OrderId   sellOrderId{};
    Price     price{0};
    Quantity  quantity{0};
    Timestamp timestamp{0};
    // The aggressor is the incoming order that crossed the book.
    Side      aggressorSide{Side::Buy};
};

} // namespace matching_engine
