# Shenzhen factor optimization following the Shanghai branch review

Reference: `sse-factor-opt-20260909` at
`9129ed31ea205746f569db3397bef5468456f9ef`.
Shenzhen baseline: `467e1c1` (the in-process Paper OMS shadow revision).
Implementation and verification use a separate checkout and build directory.

## What Shanghai changed

The main speedup comes from removing per-sample work proportional to the number
of resting orders. Each price level maintains volume, order count, insertion-time
sum and young volume as orders change. Sample-time depth generation then visits
price levels rather than all orders. An expiry queue replaced per-order time
trees. The factor path also visits depth directly and consumes streaming flow
totals, avoiding event-vector scans and repeated transcendental calculations.

The branch's component benchmark, with 50,000 resting orders, 100 price levels
and 520 records per measured batch, reports factor construction falling from
1,174.98 us to about 9.3 us through several stages. This is a synthetic component
benchmark on the Shanghai server, not live end-to-end latency. Its final flow
optimization moves some work to book updates: about 106.9 to 118.5 us per batch,
while factors fall from about 32.0 to 9.3 us and the separately measured combined
path falls from about 224.9 to 213.7 us.

Two changes need separate model-contract review before adoption: order ages are
rounded to seconds, and `1 - tanh(x)` uses an interpolation table with saturated
endpoints. The source's `HHMMSScc` clock contains fractional seconds, despite the
report's claim that second rounding loses no source precision. The report itself
records a checksum change after age rounding. A passing checksum on a fixture
does not establish universal equivalence of the interpolation approximation.

Relevant upstream files:

- `docs/sse_book_factor_opt_20260909.md`
- `sse/market_data/sse_tick_order_book.cpp`
- `sse/factors/sse_tick_factors.cpp`
- `tests/sse/sse_book_incremental_test.cpp`

## Shenzhen implementation

Shenzhen already accumulates flow incrementally. Its expensive work was the
full-order age scan inside `fill_book_factors()` and a second scan for young
volume. This change maintains insertion-time sums and young volume per price
level, expires orders through a FIFO with lazy removal of cancelled/filled
references, and reuses the two factor-depth buffers. No public runtime API,
model weights, sampling trigger, OMS policy or deployment service changes.

Age precision stays in microseconds; age exactly 30 seconds remains young.
Insertion-time sums use `__int128` because epoch-microsecond timestamps multiplied
by thousands of orders can overflow int64 even when the actual age sum fits.
Partial fills decrement young quantity; only final removal decrements the
insertion-time sum. Removed levels and stale queue references cannot subtract
from a replacement order with a different identity/time. Runtime already rejects
regressing exchange timestamps; clearing the book clears the expiry state too.

Young-volume weighting now multiplies once per level instead of once per order.
This preserves the formula but changes floating-point addition order. Therefore
the probe compares all float factor values and sample identities individually;
no precision relaxation or approximate `tanh` was added to the production code.

## Measurement and reproduction

Both variants use GCC 4.8.5, `-O3 -march=x86-64 -mtune=generic
-ffp-contract=off`, sequentially on Shenzhen CPU 44. EventTiming instrumentation
is enabled only in the diagnostic binaries. Probe CSV writing, journal I/O,
decoding and model inference are outside the component timers. The complete
probe's wall time includes its I/O and must not be described as trading latency.

The synthetic benchmark uses 100 price levels and 1,000/10,000/50,000 resting
orders, 20 warmup rounds and 200 measured rounds. Every round adds two orders and
fills both, keeping the background book unchanged. A round exceeds the amount
trigger; the time trigger in this runtime is 100 seconds, not 100 milliseconds.
Input construction and dump formatting are outside the timers. Initial-book
construction is excluded from measured-event statistics.

| Resting orders | Baseline sample p50 (us) | Optimized sample p50 (us) |
|---:|---:|---:|
| 1,000 | 2.51 | 1.49 |
| 10,000 | 56.15 | 1.50 |
| 50,000 | 279.26 | 1.49 |

All 600 synthetic samples, including identities and 30,000 factor values,
produced identical dump files.

The first 100,000,000 actual journal records from 2026-09-09 selected 742,347
events for the 20 profile instruments and produced 36,238 samples. All 1,811,900
factor values and sample identities matched exactly at zero tolerance.

| Component | Baseline p50 (us) | Optimized p50 (us) | Baseline p99 (us) | Optimized p99 (us) |
|---|---:|---:|---:|---:|
| Sampling/factor work, emitting events | 33.65 | 4.40 | 128.74 | 15.00 |
| Book mutation, selected events | 0.27 | 0.34 | 0.89 | 1.25 |
| Complete runtime callback, selected events | 0.80 | 0.88 | 59.91 | 6.56 |

Full-day replay consumed all 277,910,376 committed records on September 9,
selecting 2,033,382 events and emitting 89,167 samples. All 4,458,350 factor
values and all sample identities matched at zero tolerance (no changed values).

| Full-day component | Baseline p50 (us) | Optimized p50 (us) | Baseline p99 (us) | Optimized p99 (us) |
|---|---:|---:|---:|---:|
| Sampling/factor work, emitting events | 45.97 | 4.55 | 186.17 | 17.62 |
| Book mutation, selected events | 0.28 | 0.37 | 0.78 | 1.30 |
| Complete runtime callback, selected events | 0.81 | 0.94 | 81.60 | 6.74 |

Full-day callback mean fell from 3.306 to 1.224 us, while callback maximum was
1.104 ms baseline versus 1.399 ms optimized. Sample-time p50 improved 10.1x;
the whole probe wall time including scanning/filtering all records and writing
CSV improved only from 155.78 to 150.81 seconds. These are different metrics.
Aggregate evidence is in `benchmarks/sze-factor-20260909.json`.

`sample_work_ns` includes cut construction and sampling decisions, not just
arithmetic producing the 50 factors. This is offline replay with exchange-time
receive fallback; it does not certify current online latency or reproduce live
receive-jitter decisions. The previously quoted 146.72 us was an August 7 log
sample and is not the baseline for this change.

The expiry work is amortized, not constant in the worst case. A synthetic cohort
of 50,000 orders expiring together produced a roughly 397 us callback maximum.
The first 100M real-record run also has scheduling/expiry outliers: mutation max
0.368 ms baseline versus 0.874 ms optimized. Better sample p50/p99 is not a hard
latency bound. Further work on concentrated expiry and live measurement remains
appropriate before a real-order cutover.

Build diagnostic targets with:

```sh
cmake --build build/verify --target sze_factor_benchmark sze_factor_journal_probe
taskset -c 44 build/verify/sze_factor_benchmark --dump samples.jsonl
taskset -c 44 build/verify/sze_factor_journal_probe JOURNAL PROFILE 100000000 output.csv
python3 tools/sze/compare_factor_probe.py baseline.csv candidate.csv
```

Use the identical probe source with each runtime revision, identical profile and
record limit, and run the two variants sequentially. A nonzero probe result
means the run is invalid; never ignore failed events for benchmarking. Each dump
contains eight identity columns and 50 factor columns. A record limit above the
journal size measures the available complete recording through its committed
end. No SHM attachment or execution backend is used by the probe.

Focused aggregate/reference tests cover 30 seconds +/- 1 microsecond, partial
fills, cancellation, empty-level reuse, reset, and epoch sum overflow. The 45
registered CTest cases pass, including the real-model sequence golden test.
