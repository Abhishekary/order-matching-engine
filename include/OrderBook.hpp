#pragma once
#include <map>
#include <list>
#include <unordered_map>
#include <optional>
#include "Order.hpp"

namespace matching_engine {

// OrderBook is a pure data structure: it knows how to store, locate, mutate
// and remove resting orders with price-time priority. It performs NO
// matching logic itself -- that is the responsibility of MatchingEngine,
// which mutates the book via this API while walking the opposite side.
//
// Complexity:
//   insert / cancel / amend / lookup : O(log P) amortized, O(1) index lookup
//                                       (P = number of distinct price levels)
//   best bid/ask                     : O(1)  (map::begin())
//
// Data layout:
//   bids_ / asks_ : std::map<Price, std::list<Order>>  -- balanced BST of
//                    price levels, each holding a FIFO (time-priority) queue
//                    of resting orders at that price.
//   index_        : std::unordered_map<OrderId, Location> -- O(1) average
//                    lookup from order id straight to its list iterator,
//                    used for O(1)-after-lookup cancel/amend without
//                    scanning price levels.
//
// std::list is used (not std::deque/std::vector) for each price level
// specifically because list iterators/references remain valid after
// insertions/removals anywhere else in the container -- this is what makes
// storing raw iterators in index_ safe.
class OrderBook {
public:
    using PriceLevel = std::list<Order>;
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>; // best bid = highest price = begin()
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;    // best ask = lowest price  = begin()

    enum class AmendResult { NotFound, InPlace, Requeued };

    OrderBook() = default;

    // Insert a brand-new resting order (remainingQty must be > 0).
    void insert(const Order& order);

    // Pointer to the best (front-of-priority) price level on `side`,
    // or nullptr if that side of the book is empty.
    PriceLevel* bestLevel(Side side);

    // Best price on a given side, if any resting liquidity exists.
    std::optional<Price> bestPrice(Side side) const;
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;

    // Reduces the resting order at the FRONT of the level for `side`/`price`
    // by `qty`. Updates its status, and if it becomes fully filled, removes
    // it from the level (and the index), erasing the level itself if it is
    // now empty. Caller (MatchingEngine) is responsible for knowing which
    // level/side it is matching against and for not over-filling.
    void fillFront(Side side, Price price, Quantity qty);

    // Cancel a resting order by id. Returns true if it was found & removed.
    bool cancel(OrderId id);

    // Amend a resting order's price and/or quantity.
    //  - If price is unchanged and the new quantity does NOT exceed the
    //    current remaining quantity, the amend is applied in place and the
    //    order KEEPS its queue position (time priority preserved) -- this
    //    mirrors real exchange behavior for quantity-decrease-only amends.
    //  - Otherwise (price change, or quantity increase) the order is
    //    cancelled and re-inserted at the back of the (possibly new) price
    //    level with a fresh timestamp -- i.e. it loses time priority,
    //    exactly like a cancel/replace on a real exchange.
    AmendResult amend(OrderId id,
                       std::optional<Price> newPrice,
                       std::optional<Quantity> newQty,
                       Timestamp ts);

    const Order* find(OrderId id) const;
    Order* find(OrderId id);

    bool empty(Side side) const;
    std::size_t orderCount() const { return index_.size(); }
    std::size_t levelCount(Side side) const;

private:
    struct Location {
        Side side;
        Price price;
        PriceLevel::iterator it;
    };

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Location> index_;

    PriceLevel& levelFor(Side side, Price price);
    void eraseLevelIfEmpty(Side side, Price price);
};

} // namespace matching_engine
