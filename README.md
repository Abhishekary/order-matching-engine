# Trading Order Matching Engine

![CI](https://github.com/YOUR_USERNAME/order-matching-engine/actions/workflows/ci.yml/badge.svg)

A low-latency, in-memory limit order book and matching engine in C++17,
implementing price-time priority execution for Market and Limit orders.

## Features

- **Price-time priority matching** for Market and Limit orders
- **Order book** built on `std::map` (balanced BST) of price levels, each a
  `std::list` FIFO queue — O(log P) insert/best-price, O(1) FIFO pop
- **O(1) average order lookup/cancel** via an auxiliary `unordered_map<OrderId, iterator>` index
- **Partial fills**, order **amendments** (in-place for quantity-decrease,
  cancel/replace — losing time priority — for price change or quantity
  increase, matching real exchange semantics)
- **Trade reporting** via a callback and an in-memory trade log
- **Best Bid/Offer (BBO)** maintained implicitly by the map ordering (O(1) to read)
- **Thread-safe order ingestion**: a single mutex serializes access to the
  book so concurrent client threads can submit/cancel/amend without races,
  while execution stays fully consistent (no interleaved partial matches)
- **Benchmark harness** simulating many concurrent clients submitting
  randomized orders, reporting throughput and average submit latency

## Design

```
include/
  Order.hpp          Order struct, Side/OrderType/OrderStatus enums
  Trade.hpp           Trade (fill) report struct
  OrderBook.hpp        Pure data structure: price levels + FIFO queues + index
  MatchingEngine.hpp   Matching logic + thread-safety + trade reporting
src/
  OrderBook.cpp
  MatchingEngine.cpp
  main.cpp             Functional demo + multithreaded benchmark
tests/
  test_main.cpp         Dependency-free unit tests (price-time priority,
                         partial fills, market sweeps, cancel, amend, BBO)
```

**Why `std::map<Price, std::list<Order>>` per side?**
`std::map` keeps price levels sorted, giving O(1) access to the best price
(`begin()`) and O(log P) insertion of a new level. Each level is a
`std::list` rather than a `std::deque`/`std::vector` specifically because
list iterators/references stay valid after insertions or removals
*elsewhere* in the container — which is what lets the `unordered_map`
index store raw iterators safely for O(1) cancel/amend without a linear
scan of the book.

**Price representation.** Prices are stored as integer ticks (`int64_t`),
not `double`, to avoid floating-point comparison/precision bugs in a
matching engine — a standard practice on real exchanges.

**Matching semantics.**
- A **Limit** order matches while its price is at least as aggressive as
  the best opposite price; any unmatched remainder rests on the book.
- A **Market** order sweeps available liquidity price-level by price-level
  until filled or the book is exhausted; any unmatched remainder is
  cancelled (IOC) — market orders never rest.
- Trades always print at the **resting** order's price (price improvement
  for the aggressor), which is standard matching-engine behavior.

**Thread-safety model.** `MatchingEngine` wraps a single `OrderBook` with
one `std::mutex`. Every public entry point (`submitOrder`, `cancelOrder`,
`amendOrder`, `getBBO`, `getOrder`) takes the lock for the duration of its
critical section. This keeps matching atomic and consistent under
concurrent submission from many threads, at the cost of serializing
access to a given symbol's book (the realistic bottleneck in single-symbol
matching engines — real systems shard by symbol to scale horizontally,
which is a natural extension here: one `MatchingEngine` instance per
symbol, each with its own mutex).

## Build & Run

Requires g++ with C++17 support (no external dependencies, no CMake needed).

```bash
make run     # builds and runs the functional demo + benchmark
make test    # builds and runs the unit tests
make clean
```

Benchmark thread/order count can be overridden:

```bash
./build/matching_engine <numThreads> <ordersPerThread>
```

### Sample benchmark output

On a multi-core dev machine, 2 threads x 20,000 orders each against a
seeded ~4,000-order book:

```
Orders submitted   : 40000
Trades generated   : ~32000
Wall time          : ~0.02 s
Throughput         : ~2,000,000+ orders/sec
Avg submit latency : ~0.7 us
```

Throughput scales with core count up to lock contention on the single
book mutex; the natural next optimization (see below) is sharding by
symbol or moving to a lock-free/ single-writer ring-buffer ingestion
model for further latency reduction.

## Possible Extensions

- Multi-symbol support: `unordered_map<Symbol, MatchingEngine>`
- Lock-free SPSC/MPSC ingestion queue feeding a single matching thread
  (removes lock contention entirely, closer to real exchange architecture)
- Order types: Stop, Stop-Limit, Fill-Or-Kill, Immediate-Or-Cancel for limit orders
- Persistence / event sourcing of the order log for replay and audit
- Market data feed: incremental BBO/depth-of-book change events
