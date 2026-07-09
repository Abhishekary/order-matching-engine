# Trading Order Matching Engine

A low-latency, in-memory limit order book and matching engine written in C++17, which implements price-time priority execution for Market and Limit orders.

## Features

- **Price-time priority matching** for Market and Limit orders
- **Order book** built on `std::map` (balanced BST) of price levels, each being a `std::list` FIFO queue; O(log P) for insert/best-price, O(1) for FIFO pop
- **O(1) average order lookup/cancel** using an auxiliary `unordered_map<OrderId, iterator>` index
- **Partial fills, order amendments** (in-place for quantity decrease, cancel/replace losing time priority for price change or quantity increase, matching real exchange behaviors)
- **Trade reporting** through a callback and an in-memory trade log
- **Best Bid/Offer (BBO)** maintained implicitly by the map ordering (O(1) to read)
- **Thread-safe order ingestion,** where a single mutex serializes access to the book, allowing concurrent client threads to submit, cancel, or amend without conflicts, while ensuring execution remains consistent (no interleaved partial matches)
- **Benchmark harness** that simulates many concurrent clients submitting randomized orders, reporting throughput and average submit latency

## Design

```
include/
  Order.hpp          Order struct, Side/OrderType/OrderStatus enums
  Trade.hpp           Trade (fill) report struct
  OrderBook.hpp        Pure data structure: price levels, FIFO queues, and index
  MatchingEngine.hpp   Matching logic, thread-safety, and trade reporting
src/
  OrderBook.cpp
  MatchingEngine.cpp
  main.cpp             Functional demo and multithreaded benchmark
tests/
  test_main.cpp         Dependency-free unit tests (price-time priority, partial fills, market sweeps, cancel, amend, BBO)
```

**Why `std::map<Price, std::list<Order>>` per side?**  
`std::map` keeps price levels sorted. This allows O(1) access to the best price (`begin()`) and O(log P) insertion of a new level. Each level uses a `std::list` over a `std::deque` or `std::vector` because list iterators remain valid after insertions or removals elsewhere in the container. This feature lets the `unordered_map` index store raw iterators safely for O(1) cancel/amend without needing to scan the book linearly.

**Price representation.**  
Prices are stored as integer ticks (`int64_t`), not `double`, to avoid floating-point comparison and precision issues in a matching engine. This practice is common in real exchanges.

**Matching semantics.**  
- A **Limit** order matches when its price is at least as aggressive as the best opposite price; any unmatched remainder is left on the book.
- A **Market** order sweeps available liquidity price level by price level until it is filled or the book runs out; any unmatched remainder is canceled (IOC) since market orders never rest.
- Trades always print at the price of the **resting** order (providing price improvement for the aggressor), which is standard behavior for matching engines.

**Thread-safety model.**  
`MatchingEngine` wraps a single `OrderBook` with one `std::mutex`. Every public function (`submitOrder`, `cancelOrder`, `amendOrder`, `getBBO`, `getOrder`) acquires the lock during its critical section. This ensures that matching is atomic and consistent during concurrent submissions from multiple threads, but it does serialize access to a specific symbol's book. This serialization can be a bottleneck in single-symbol matching engines. In actual systems, they shard by symbol to scale; this could also be a future step: one `MatchingEngine` instance per symbol, each having its own mutex.

## Build & Run

This project needs g++ with C++17 support. There are no external dependencies and no CMake required.

```bash
make run     # builds and runs the functional demo and benchmark
make test    # builds and runs the unit tests
make clean
```

You can change the benchmark thread/order count:

```bash
./build/matching_engine <numThreads> <ordersPerThread>
```

### Sample benchmark output

On a multi-core developer machine, using 2 threads with 20,000 orders each against a seeded ~4,000-order book:

```
Orders submitted   : 40000
Trades generated   : ~32000
Wall time          : ~0.02 s
Throughput         : ~2,000,000+ orders/sec
Avg submit latency : ~0.7 us
```

Throughput scales with the number of cores until hitting lock contention on the single book mutex. The next natural optimization is to shard by symbol or shift to a lock-free/single-writer ring-buffer ingestion model to reduce latency further.

## Possible Extensions

- Support for multiple symbols: `unordered_map<Symbol, MatchingEngine>`
- Lock-free SPSC/MPSC ingestion queue directing to a single matching thread (removes lock contention altogether, making it closer to real exchange architecture)
- Additional order types: Stop, Stop-Limit, Fill-Or-Kill, Immediate-Or-Cancel for limit orders
- Persistence and event sourcing of the order log for replay and audit
- Market data feed: incremental BBO/depth-of-book change events
