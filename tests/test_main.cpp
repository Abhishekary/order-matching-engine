// Minimal, dependency-free test harness (no gtest required).
// Each TEST_CASE prints PASS/FAIL; main() returns non-zero if any failed.
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cassert>
#include "MatchingEngine.hpp"

using namespace matching_engine;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "  [FAIL] " << msg << "\n";
        ++g_failures;
    }
}

void run(const std::string& name, const std::function<void()>& fn) {
    std::cout << "TEST: " << name << "\n";
    fn();
}

void test_basic_limit_match() {
    MatchingEngine engine;
    engine.submitOrder(Side::Sell, OrderType::Limit, 100, 10); // resting ask id=1
    auto r = engine.submitOrder(Side::Buy, OrderType::Limit, 100, 10); // crosses fully
    check(r.trades.size() == 1, "one trade generated");
    check(r.trades[0].quantity == 10, "trade quantity matches");
    check(r.trades[0].price == 100, "trade prints at resting price");
    check(r.finalStatus == OrderStatus::Filled, "incoming order fully filled");
    check(!engine.getOrder(1).has_value(), "resting order removed after full fill");
}

void test_partial_fill_and_rest() {
    MatchingEngine engine;
    engine.submitOrder(Side::Sell, OrderType::Limit, 100, 5); // id=1, qty 5
    auto r = engine.submitOrder(Side::Buy, OrderType::Limit, 100, 12); // id=2, qty 12
    check(r.trades.size() == 1, "one trade against the only resting ask");
    check(r.trades[0].quantity == 5, "trade fills only the resting quantity");
    check(r.finalStatus == OrderStatus::PartiallyFilled, "incoming partially filled");
    auto resting = engine.getOrder(2);
    check(resting.has_value(), "remainder rests in the book");
    check(resting && resting->remainingQty == 7, "remaining qty is 12-5=7");
}

void test_price_time_priority() {
    MatchingEngine engine;
    engine.submitOrder(Side::Sell, OrderType::Limit, 100, 5); // id=1, first at price 100
    engine.submitOrder(Side::Sell, OrderType::Limit, 100, 5); // id=2, second at price 100
    auto r = engine.submitOrder(Side::Buy, OrderType::Limit, 100, 5);
    check(r.trades.size() == 1, "single trade");
    check(r.trades[0].sellOrderId == 1, "earlier order at same price fills first (FIFO)");
}

void test_price_priority_over_time() {
    MatchingEngine engine;
    engine.submitOrder(Side::Sell, OrderType::Limit, 101, 5); // id=1, worse price, earlier
    engine.submitOrder(Side::Sell, OrderType::Limit, 100, 5); // id=2, better price, later
    auto r = engine.submitOrder(Side::Buy, OrderType::Limit, 101, 5);
    check(r.trades.size() == 1, "single trade");
    check(r.trades[0].sellOrderId == 2, "better price fills before earlier worse price");
    check(r.trades[0].price == 100, "trade prints at the better resting price");
}

void test_market_order_sweeps_multiple_levels() {
    MatchingEngine engine;
    engine.submitOrder(Side::Sell, OrderType::Limit, 100, 5);
    engine.submitOrder(Side::Sell, OrderType::Limit, 101, 5);
    auto r = engine.submitOrder(Side::Buy, OrderType::Market, 0, 8);
    check(r.trades.size() == 2, "market order sweeps two price levels");
    check(r.trades[0].price == 100 && r.trades[0].quantity == 5, "first fill exhausts best level");
    check(r.trades[1].price == 101 && r.trades[1].quantity == 3, "second fill takes remainder from next level");
    check(r.finalStatus == OrderStatus::Filled, "market order fully filled");
}

void test_market_order_partial_no_rest() {
    MatchingEngine engine;
    engine.submitOrder(Side::Sell, OrderType::Limit, 100, 3);
    auto r = engine.submitOrder(Side::Buy, OrderType::Market, 0, 10);
    check(r.trades.size() == 1, "only available liquidity trades");
    check(r.finalStatus == OrderStatus::PartiallyFilled, "market remainder is not filled...");
    check(!engine.getOrder(r.id).has_value(), "...and does NOT rest in the book (IOC)");
}

void test_cancel() {
    MatchingEngine engine;
    engine.submitOrder(Side::Buy, OrderType::Limit, 99, 10); // id=1
    check(engine.cancelOrder(1), "cancel succeeds for a resting order");
    check(!engine.cancelOrder(1), "double-cancel fails");
    check(!engine.getOrder(1).has_value(), "cancelled order no longer retrievable");
}

void test_amend_qty_decrease_keeps_priority() {
    MatchingEngine engine;
    engine.submitOrder(Side::Buy, OrderType::Limit, 99, 10); // id=1
    engine.submitOrder(Side::Buy, OrderType::Limit, 99, 10); // id=2
    auto res = engine.amendOrder(1, std::nullopt, Quantity(4));
    check(res == OrderBook::AmendResult::InPlace, "qty-decrease amend applied in place");
    auto o = engine.getOrder(1);
    check(o && o->remainingQty == 4, "quantity reduced to 4");

    // id=1 should still be ahead of id=2 in the queue (priority preserved).
    auto r = engine.submitOrder(Side::Sell, OrderType::Limit, 99, 4);
    check(r.trades.size() == 1 && r.trades[0].buyOrderId == 1,
          "amended order retains time priority over id=2");
}

void test_amend_price_change_requeues() {
    MatchingEngine engine;
    engine.submitOrder(Side::Buy, OrderType::Limit, 99, 10); // id=1
    auto res = engine.amendOrder(1, Price(98), std::nullopt);
    check(res == OrderBook::AmendResult::Requeued, "price change amend requeues the order");
    auto o = engine.getOrder(1);
    check(o && o->price == 98, "price updated to 98");
    auto bbo = engine.getBBO();
    check(bbo.bid.has_value() && *bbo.bid == 98, "BBO reflects the amended price");
}

void test_bbo_maintenance() {
    MatchingEngine engine;
    check(!engine.getBBO().bid.has_value() && !engine.getBBO().ask.has_value(), "empty book has no BBO");
    engine.submitOrder(Side::Buy, OrderType::Limit, 99, 10);
    engine.submitOrder(Side::Buy, OrderType::Limit, 100, 10); // better bid
    engine.submitOrder(Side::Sell, OrderType::Limit, 103, 10);
    engine.submitOrder(Side::Sell, OrderType::Limit, 102, 10); // better ask
    auto bbo = engine.getBBO();
    check(bbo.bid && *bbo.bid == 100, "best bid is the highest resting buy price");
    check(bbo.ask && *bbo.ask == 102, "best ask is the lowest resting sell price");
}

} // namespace

int main() {
    run("basic_limit_match", test_basic_limit_match);
    run("partial_fill_and_rest", test_partial_fill_and_rest);
    run("price_time_priority", test_price_time_priority);
    run("price_priority_over_time", test_price_priority_over_time);
    run("market_order_sweeps_multiple_levels", test_market_order_sweeps_multiple_levels);
    run("market_order_partial_no_rest", test_market_order_partial_no_rest);
    run("cancel", test_cancel);
    run("amend_qty_decrease_keeps_priority", test_amend_qty_decrease_keeps_priority);
    run("amend_price_change_requeues", test_amend_price_change_requeues);
    run("bbo_maintenance", test_bbo_maintenance);

    if (g_failures == 0) {
        std::cout << "\nALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " CHECK(S) FAILED\n";
    return 1;
}
