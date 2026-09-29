# SSE v0.6 selected replay latency path (2026-09-20)

The accepted experiment reduced paced, full-universe replay receive-origin to
strategy-entry P50 from 204.192 us to 115.376 us. P95 changed from 359.301 us to
192.092 us; P99 changed from 1193.018 us to 1067.176 us. The 100 us P50 target
was not reached. These measurements exclude NIC reception and ATP transmission.

## Retained implementation

- Keep each stock on its fixed book/factor worker. Route validated raw records
  to that worker instead of fully decoding them on the ingress thread.
- Preserve the hardware BatchEnd fence, the sampling gate and per-stock order.
- Store ordered price levels contiguously with the existing Boost flat_map.
- Prepare current book aggregates only during worker idle time, and reuse them
  only when the book, event time, midpoint and factor mode still match.
- Give each book worker a model thread on CPU `worker_cpu + 1`. Only output
  jobs enter the model queue. Publish each completed prediction immediately.
- Use 16 input slots per worker, with backpressure when full. A slot count is a
  capacity limit, never a minimum number of events to wait for.
- Use a bounded 4096-slot SHM prefetch queue. Payloads are borrowed until the
  application's next read. The production entry and replay tool use the same
  `ShmPrefetch` implementation. Continuity, producer health and generation checks
  remain in `ReplayHandoffConsumer`; overruns still return to journal recovery.

The unselected notification, broadcast, shared-packet-lease, speculative full
prediction, direct-read and expanded idle-preparation variants are not included.
Legacy models and strategies remain available. v0.6 parallel dispatch always
uses the selected model-thread path; no experimental model-thread switch remains.

## Build

Use GCC 11 and a Release build on the target CPU:

```sh
cmake -S . -B build/sse-release \
  -DCMAKE_CXX_COMPILER=/opt/rh/devtoolset-11/root/usr/bin/g++ \
  -DCMAKE_BUILD_TYPE=Release -DSZE_MARCH_NATIVE=ON
cmake --build build/sse-release -j4
(cd build/sse-release && ctest --output-on-failure)
cmake --build build/sse-release --target sse_latency_replay -j4
```

Floating-point contraction remains disabled; explicit existing matrix intrinsics
are unchanged. No model weights, daily account configuration or market data are
included in the repository.

For ATP deployment, the prediction process and TD plugin must be built with the
same C++ ABI. The benchmark uses GCC 11's default ABI. Do not combine that binary
with an existing old-ABI TD plugin. The standalone build does not replace the
installed trading binaries or services.

## Runtime CPU assignment

The replay uses reader 128, ingress 136, strategy 137; book workers
144,152,160,168,176,184,192,200,208,216,224,232,240,248; corresponding model
workers use each following core. The deployment template uses `SSE_SHM_READER_CPU=129`,
leaving CPU 128 to the existing capture control process. The reader gets its own L3 lease; each model helper uses its book
worker's leased L3 on a distinct core. The ingress validates this placement.

`deploy/sse/sse-journal-trading.service` supplies the reader setting and the
expanded CPU affinity. The IRQ policy also protects the reader and model helper
cores. Actual service installation and activation are separate
from publishing this source branch.

## Reproduce the replay

The tool takes a history chain, active capture configuration, paper profile,
output directory and source-clock start/end bounds. Create the output directory
before running. The publisher is pinned to CPU 16; reader to 128.

```sh
mkdir -p "$REPLAY_OUTPUT"
SSE_AUDIT_CPU=48 SSE_OMS_JOURNAL_CPU=56 SSE_V06_AUDIT_PATH=/dev/null \
SSE_PREDICTION_CPUS=144,152,160,168,176,184,192,200,208,216,224,232,240,248 \
SSE_STRATEGY_CPU=137 taskset -c 136 build/sse-release/sse_latency_replay \
  "$HISTORY_CHAIN" "$ACTIVE_CAPTURE" "$PAPER_PROFILE" "$REPLAY_OUTPUT" \
  92093601776244 92155601976970
python tests/sse/analyze_latency_replay.py "$EVIDENCE_ROOT" "$RUN_NAME"
```

The analyzer expects `$EVIDENCE_ROOT/baseline` and `$EVIDENCE_ROOT/$RUN_NAME`.
The accepted case restores 23,819,663 historical inputs, then measures 92,390
inputs over approximately 62 seconds: 20,734 predictions and 20,578 strategy
entries across 2,318 stocks. Verification compares per-stock output counts and
bit-level factor/prediction digests, plus all 20 paper order/cancel intents
excluding their runtime clock field. Global inter-stock scheduling may differ.

Timing hooks are compiled only into `sse_latency_replay` with
`SSE_REPLAY_PROBE`. The replay compiles the production sources directly; there
is no separately maintained dispatcher or stream-processor implementation.
Normal executable and library targets contain no replay trace calls.

## Final release verification

The cleaned release shares the existing SHM mapping with its prefetch reader.
Its complete replay retained all 2,318 per-stock output digests and all 20 paper
intents. CTest passed 66/66 cases; CPU policy tests passed 5/5. The SHM tests
cover borrowed-buffer stability across capture-ring wrap, full-queue shutdown,
journal fallback, restart and generation mismatch.

| Build/run | P50 us | P95 us | P99 us |
| --- | ---: | ---: | ---: |
| Original baseline | 204.192 | 359.301 | 1193.018 |
| Selected experiment | 115.376 | 192.092 | 1067.176 |
| Selected experiment repeat | 114.829 | 193.038 | 1077.049 |
| Cleaned release | 111.438 | 188.487 | 1094.318 |

The final run reduces P50 by 45.4% from the original baseline. Per-stage
statistics and parity results are in `sse-replay-latency-20260920.json` beside
this report. Publication uploads source only; it does not replace the running
capture/prediction binaries or activate a systemd unit.
