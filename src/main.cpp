#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include "MatchingEngine.hpp"

using namespace matching_engine;

namespace {

// Prices are represented as integer ticks. Here we use 1 tick = $0.01,
// so a $100.25 price is stored as 10025.
constexpr Price TICK = 100; // $1.00 == 100 ticks ($0.01 resolution)

std::string fmtPrice(Price p) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << (static_cast<double>(p) / TICK);
    return oss.str();
}

void printBBO(const MatchingEngine& engine) {
    auto bbo = engine.getBBO();
    std::cout << "BBO  bid=" << (bbo.bid ? fmtPrice(*bbo.bid) : std::string("--"))
              << "  ask=" << (bbo.ask ? fmtPrice(*bbo.ask) : std::string("--")) << "\n";
}

void runDemo() {
    std::cout << "=== Functional demo ===\n";
    MatchingEngine engine([](const Trade& t) {
        std::cout << "  TRADE  buy=" << t.buyOrderId << " sell=" << t.sellOrderId
                  << " qty=" << t.quantity << " price=" << fmtPrice(t.price) << "\n";
    });

    // Build up resting liquidity on both sides.
    engine.submitOrder(Side::Buy, OrderType::Limit, 99 * TICK, 100);   // id 1
    engine.submitOrder(Side::Buy, OrderType::Limit, 100 * TICK, 50);   // id 2 (best bid)
    engine.submitOrder(Side::Sell, OrderType::Limit, 102 * TICK, 75);  // id 3 (best ask)
    engine.submitOrder(Side::Sell, OrderType::Limit, 103 * TICK, 120); // id 4
    printBBO(engine);

    std::cout << "-- Limit buy that partially crosses the book --\n";
    auto r1 = engine.submitOrder(Side::Buy, OrderType::Limit, 102 * TICK, 100); // id 5
    std::cout << "order " << r1.id << " final status="
              << static_cast<int>(r1.finalStatus) << ", trades=" << r1.trades.size() << "\n";
    printBBO(engine);

    std::cout << "-- Market sell sweeping remaining bid liquidity --\n";
    auto r2 = engine.submitOrder(Side::Sell, OrderType::Market, 0, 200); // id 6
    std::cout << "order " << r2.id << " final status="
              << static_cast<int>(r2.finalStatus) << ", trades=" << r2.trades.size() << "\n";
    printBBO(engine);

    std::cout << "-- Cancel + amend --\n";
    auto r3 = engine.submitOrder(Side::Sell, OrderType::Limit, 104 * TICK, 60); // id 7, still resting
    bool cancelled = engine.cancelOrder(r3.id);
    std::cout << "cancel(" << r3.id << ") => " << std::boolalpha << cancelled << "\n";

    auto r4 = engine.submitOrder(Side::Buy, OrderType::Limit, 95 * TICK, 40); // id 8, still resting
    auto amendRes = engine.amendOrder(r4.id, std::nullopt, Quantity(25)); // qty-decrease: in-place
    std::cout << "amend(" << r4.id << ", qty=25) => in-place? "
              << (amendRes == OrderBook::AmendResult::InPlace) << "\n";
    auto amended = engine.getOrder(r4.id);
    std::cout << "  order " << r4.id << " remaining qty now = "
              << (amended ? amended->remainingQty : 0) << "\n";
    printBBO(engine);

    std::cout << "Total trades printed so far: " << engine.totalTrades() << "\n\n";
}

struct BenchStats {
    std::atomic<uint64_t> ordersSubmitted{0};
    std::atomic<uint64_t> tradesGenerated{0};
    std::atomic<uint64_t> totalLatencyNs{0};
};

void producerThread(MatchingEngine& engine, int threadId, int ordersPerThread, BenchStats& stats) {
    std::mt19937_64 rng(std::random_device{}() + threadId);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<int> typeDist(0, 9); // 10% market orders
    std::uniform_int_distribution<Price> priceOffsetDist(-25, 25); // +/- $0.25 around mid
    std::uniform_int_distribution<Quantity> qtyDist(1, 50);

    const Price mid = 100 * TICK;

    for (int i = 0; i < ordersPerThread; ++i) {
        Side side = sideDist(rng) == 0 ? Side::Buy : Side::Sell;
        OrderType type = (typeDist(rng) == 0) ? OrderType::Market : OrderType::Limit;
        Price price = mid + priceOffsetDist(rng);
        Quantity qty = qtyDist(rng);

        auto start = std::chrono::steady_clock::now();
        auto result = engine.submitOrder(side, type, price, qty);
        auto end = std::chrono::steady_clock::now();

        stats.ordersSubmitted.fetch_add(1, std::memory_order_relaxed);
        stats.tradesGenerated.fetch_add(result.trades.size(), std::memory_order_relaxed);
        stats.totalLatencyNs.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(),
            std::memory_order_relaxed);
    }
}

void runBenchmark(int numThreads, int ordersPerThread) {
    std::cout << "=== Benchmark: " << numThreads << " threads x " << ordersPerThread
              << " orders/thread ===\n";

    MatchingEngine engine; // no trade callback -- keep the hot path lean
    BenchStats stats;

    // Seed the book so incoming orders have something to match against.
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price> seedOffset(-50, -1);
    std::uniform_int_distribution<Price> seedOffsetAsk(1, 50);
    std::uniform_int_distribution<Quantity> seedQty(10, 200);
    for (int i = 0; i < 2000; ++i) {
        engine.submitOrder(Side::Buy, OrderType::Limit, 100 * TICK + seedOffset(rng), seedQty(rng));
        engine.submitOrder(Side::Sell, OrderType::Limit, 100 * TICK + seedOffsetAsk(rng), seedQty(rng));
    }

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    auto wallStart = std::chrono::steady_clock::now();
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back(producerThread, std::ref(engine), t, ordersPerThread, std::ref(stats));
    }
    for (auto& th : threads) th.join();
    auto wallEnd = std::chrono::steady_clock::now();

    const double wallSeconds = std::chrono::duration<double>(wallEnd - wallStart).count();
    const uint64_t submitted = stats.ordersSubmitted.load();
    const double avgLatencyUs = (static_cast<double>(stats.totalLatencyNs.load()) / submitted) / 1000.0;
    const double throughput = submitted / wallSeconds;

    std::cout << "Orders submitted   : " << submitted << "\n";
    std::cout << "Trades generated   : " << stats.tradesGenerated.load() << "\n";
    std::cout << "Wall time          : " << std::fixed << std::setprecision(3) << wallSeconds << " s\n";
    std::cout << "Throughput         : " << std::fixed << std::setprecision(0) << throughput << " orders/sec\n";
    std::cout << "Avg submit latency : " << std::fixed << std::setprecision(2) << avgLatencyUs << " us\n";
    printBBO(engine);
    std::cout << "\n";
}

} // namespace

int main(int argc, char** argv) {
    runDemo();

    int numThreads = std::max(2u, std::thread::hardware_concurrency());
    int ordersPerThread = 20000;
    if (argc >= 2) numThreads = std::atoi(argv[1]);
    if (argc >= 3) ordersPerThread = std::atoi(argv[2]);

    runBenchmark(numThreads, ordersPerThread);
    return 0;
}
