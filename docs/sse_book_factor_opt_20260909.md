# SSE order-book and factor optimization

Baseline: `sse-dev` at `77c8f87`. Scope is the Shanghai tick order book and
50-factor path only. Shenzhen, decoder runtime, and production services were
not changed.

## Implementation

- `OrderBook` keeps one ordered price map per side. Each price entry updates
  quantity, live order count, add-time sum, and young quantity on add, cancel,
  and trade. `full_depth()` now emits that aggregate directly and does not
  scan `orders_` or build a per-call price index.
- Order age is stored in exchange seconds. The wire time is only `HHMMSScc`,
  so this deliberately removes false microsecond precision; the aggregate
  factor interface still exposes the historical microsecond field in multiples
  of one second. Exact age 30 seconds remains young and age 31 seconds is not.
- A compact append-only age queue replaces the live-time and active-young
  red-black trees. Cancel/fill leaves a cheap stale reference; a batch-end or
  full-depth query advances activation and expiry cursors and skips references
  whose order was already removed. Periodic prefix compaction bounds memory.
  A query-clock rollback rebuilds young quantities by scanning live orders and
  resets the cursors, preserving the existing exceptional O(N_live) fallback.
- `apply(const sse_live::DecodedTick&)` shares the existing event logic through
  a private template, so the fixed-layout POD reaches the book without a
  temporary `std::string`/`TickEvent` conversion. The legacy overload remains.
- `take_flow_window(FlowStats*)` allows the factor state to retain the flow
  event vector. The return-by-value overload remains for existing callers.
- The hot cancel/fill path tests the cached young predicate directly and does
  not touch an age tree for every quantity update.

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
`make -j4`. The pre-seconds baseline checksum was
`71890.4863182008`; the seconds/lazy-age and streaming-flow path is
`71890.9272733331` (the documented relative difference is from the deliberate
seconds precision change).

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
| 1,000 | 20 | 4.15 -> 6.62 | 57.49 -> 13.39 | 46.94 -> 4.73 |
| 1,000 | 520 | 107.62 -> 168.72 | 77.18 -> 32.22 | 46.96 -> 4.73 |
| 10,000 | 20 | 4.24 -> 6.95 | 256.76 -> 13.40 | 246.58 -> 4.74 |
| 10,000 | 520 | 108.41 -> 177.04 | 276.64 -> 32.16 | 246.15 -> 4.73 |
| 50,000 | 20 | 4.34 -> 7.24 | 1,152.55 -> 13.58 | 1,141.44 -> 4.74 |
| 50,000 | 520 | 108.13 -> 183.62 | 1,174.98 -> 32.39 | 1,144.28 -> 4.74 |

The original update increase was largely the cost of maintaining the exact
ordered live-time index and per-level aggregates. The seconds/expiry-queue
implementation below must be measured separately. The benchmark is a controlled component test,
not raw decode, socket/SHM, routing, queue, model, full-history replay, or
full-day live throughput; these numbers do not establish production latency.

## Seconds and expiry-queue follow-up

The follow-up keeps the public `Level.add_time_sum_micros` field for
compatibility, but stores order age internally as exchange seconds. The input
wire time is `HHMMSScc`, so this does not discard precision present in the
Shanghai source. The two per-order time trees were replaced by an append-only
age queue with lazy stale-reference removal and periodic prefix compaction.

The same CPU56 controlled fixture was rerun after the change. Median
microseconds for 50,000 resting orders and 520 records were:

| Component | Before seconds change | Seconds queue |
|---|---:|---:|
| Order-book updates | 183.62 | 106.89 |
| Factor build | 32.39 | 31.95 |
| Tick inference | 85.70 | 85.82 |
| Combined measured path | 302.56 | 224.86 |

The combined measurement is taken around the complete component path and is
not the sum of component percentiles. The old/new real-model fixture checksum
changed from `71890.4863` to `71890.9273`, a relative difference of about
`0.00061%`; serial and parallel pipeline outputs remained exactly equal. The
seconds boundary test and all 50 integrated CTest targets passed.

## Streaming flow and zero-copy full-depth aggregation

The next bottleneck was the flow loop inside `FactorState::build()`. For a
520-record window, the loop scanned every retained event and made roughly 300
price-distance `tanh` calls. A controlled split measured the flow scan at about
19.5µs; the rest of the factor build was about 12.6µs.

Flow is now accumulated as the book applies each add/trade. The book keeps the
raw order/fill quantities and model-share flow totals for positive, negative,
market, and trade flow. The factor path reads those counters in constant time;
the event vector is still retained for `has_flow` and compatibility consumers,
but it is no longer rescanned. The hot price weight uses a 2048-entry linear
interpolation table for `1 - tanh(x)` on `[-4,4]`, with saturated endpoints
outside the interval. The table is initialized when an `OrderBook` is created,
so the first market update does not pay the initialization cost.

Full-depth factor construction now visits the ordered price maps directly and
feeds the existing one-pass band accumulator. The public `full_depth()` vector
API remains available, while the factor path avoids copying both sides into
temporary vectors. This also keeps the fixed-depth and distance-band formulas
on one pass over each side.

On the same CPU56 synthetic fixture, the representative 50,000-order,
520-record case is:

| Component | Seconds/lazy-age path | Streaming flow + direct depth |
|---|---:|---:|
| Order-book updates | ~106.9µs | ~118.5µs |
| Factor build | ~32.0µs | ~9.3µs |
| Tick inference | ~85.8µs | ~85.6µs |
| Combined path | ~224.9µs | ~213.7µs |

The extra book time contains the streaming flow arithmetic; the combined path
is lower because the former 19.5µs event scan and the vector materialization
are gone. The standalone diagnostic `full_depth()` API remains around 4.75µs
for 100 levels; factor construction no longer pays that vector-copy cost.
The real-model checksum remains `71890.9272733331`, and serial/parallel
pipeline outputs remain exactly equal. The full 50-target CTest run passes.
The measured JSON is in
`docs/benchmarks/sse-flow-streaming-20260909.json`.

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
