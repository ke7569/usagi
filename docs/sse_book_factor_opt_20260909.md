# SSE order-book and factor optimization

Baseline: `sse-dev` at `77c8f87`. Scope is the Shanghai tick order book and
50-factor path only. Shenzhen, decoder runtime, and production services were
not changed.

## Implementation

- `OrderBook` keeps one ordered price map per side. Each price entry updates
  quantity, live order count, add-time sum, and young quantity on add, cancel,
  and trade. `full_depth()` now emits that aggregate directly and does not
  scan `orders_` or build a per-call price index.
- A live `(add_time_micros, order_no)` index is exact-erased when an order is
  fully canceled or filled. A second bounded active-young set handles forward
  query-time expiry. Expiry is strict: age exactly 30,000,000 microseconds is
  young, and age 30,000,001 is not. A query-clock rollback rebuilds from the
  live time index so future-dated orders remain excluded by `now >= add_time`.
- With the normal nondecreasing query clock, young-cache maintenance visits
  only newly eligible and newly expired live-time entries (plus the ordered
  price levels emitted by `full_depth()`). The first query when no cache exists,
  or any query-clock rollback, intentionally has an O(N_live) rebuild over the
  bounded live-time index to preserve exact arbitrary-query behavior; this is
  an exceptional fallback, not a claim that every query avoids all-order work.
- `apply(const sse_live::DecodedTick&)` shares the existing event logic through
  a private template, so the fixed-layout POD reaches the book without a
  temporary `std::string`/`TickEvent` conversion. The legacy overload remains.
- `take_flow_window(FlowStats*)` allows the factor state to retain the flow
  event vector. The return-by-value overload remains for existing callers.

## Correctness checks

`tests/sse/sse_book_incremental_test.cpp` compares every checked cut against a
slow order-by-order reference and covers multiple price levels, same-price
orders, partial cancellation, partial two-sided trade, last-order level
removal, query times before/at/after the 30-second boundary, clock rollback,
and direct `DecodedTick` application. It also checks the reusable flow API.

The focused test was built and run with GCC 4.8.5:

```text
g++ -std=c++11 -O2 -Wall -Wextra -Werror -I. \
  tests/sse/sse_book_incremental_test.cpp /tmp/sse_tick_order_book.o \
  -o /tmp/sse_book_incremental_test
/tmp/sse_book_incremental_test
```

The complete `sse_stream_processing` target also built successfully with
`make -j4`. The old and new downstream benchmark checksums both were
`71890.4863182008`.

## Controlled benchmark

The existing `sse_downstream_benchmark.cpp` was compiled once against the
baseline `build/sse-hardware` library and once against this branch's library,
with the same profile and compiler flags. Both runs were serialized on CPU
72 (the CPU 72-79 L3 group), using 20 warmup and 200 measured iterations per
case. The fixture has one synthetic stock, 100 occupied price levels, and
1,000/10,000/50,000 resting orders. Each measured batch has 20 or 520 valid
records and preserves the resting order count.

Median microseconds (`old -> new`):

| Resting orders | Records | Book update | Factor build | B/S full-depth only |
|---:|---:|---:|---:|---:|
| 1,000 | 20 | 4.15 -> 7.24 | 57.49 -> 13.37 | 46.94 -> 4.79 |
| 1,000 | 520 | 107.62 -> 184.69 | 77.18 -> 32.17 | 46.96 -> 4.78 |
| 10,000 | 20 | 4.24 -> 7.93 | 256.76 -> 13.37 | 246.58 -> 4.79 |
| 10,000 | 520 | 108.41 -> 202.55 | 276.64 -> 32.19 | 246.15 -> 4.79 |
| 50,000 | 20 | 4.34 -> 8.46 | 1,152.55 -> 13.56 | 1,141.44 -> 4.78 |
| 50,000 | 520 | 108.13 -> 215.24 | 1,174.98 -> 32.40 | 1,144.28 -> 4.78 |

The update increase is the cost of maintaining the exact ordered live-time
index and per-level aggregates. The benchmark is a controlled component test,
not raw decode, socket/SHM, routing, queue, model, full-history replay, or
full-day live throughput; these numbers do not establish production latency.

## CMake integration fragment

The focused test was intentionally left out of the shared CMake file to avoid
conflicting with other worker changes. A merge can register it after the
`sse_stream_processing` target is available:

```cmake
add_executable(sse_book_incremental_test
  ${USAGI_ROOT}/tests/sse/sse_book_incremental_test.cpp)
target_link_libraries(sse_book_incremental_test PRIVATE sse_stream_processing)
target_compile_options(sse_book_incremental_test PRIVATE -UNDEBUG)
add_test(NAME sse_book_incremental_test COMMAND sse_book_incremental_test)
```
