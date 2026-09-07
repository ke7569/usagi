# Shanghai In-Memory Event Pipeline

`sse_event_pipeline` is the Shanghai capture and sharded model entry point.
It supports the tick model and optional opening snapshot ensemble, with an
order-book validation consumer when no model is configured. It does not load
TD or replace the running trading process. Shenzhen code is unchanged.

## Runtime

```text
UDP receive workers -> individual SPSC ingress queues -> dispatcher
                                                        |-> shard 0 queue -> book consumer
                                                        |-> shard N queue -> book consumer
                                                       `-> journal queue -> journal writer
```

Receive callbacks decode wire records and publish fixed-size memory events.
They do not format CSV, append journal records, wait for disk, or call the
strategy framework. Every producer has one preallocated SPSC queue. The
dispatcher is the sole owner of the shard map and each downstream queue's
producer position. Each shard is the sole owner of its stock states.

Queue pushes never block. An ingress or shard overflow invalidates computation.
A journal overflow invalidates persistence and stops adding the discontinuous
suffix; live queues continue independently. An unresolved snapshot-buffer
overflow invalidates computation while the event can still be journaled.
These states are reported explicitly; they are not permission to trade.

The dispatcher maintains one replaceable quiet-period timer per stock. When
more than 100us of source receive time passes, it emits `kTickSample` before
the next event, or while live input is idle. The marker carries the prior tick
payload, stable channel/shard, and its own event ID; it is journaled with the
market events. Replay consumes those markers without recalculating sampling
from wall-clock playback speed. Stop drains the last ingress events and then
closes pending sample windows, following the prior parallel-live behavior.

## Event and Stock Ownership

`Event` version 1 is a fixed 504-byte little-endian POD envelope containing
event ID, original realtime/monotonic receive timestamps, exchange time,
exchange sequence, trading day, ChannelNo, shard ID, six-digit security ID,
kind, payload length, and the complete 72/440-byte wire record.
Kinds are tick=1, snapshot=2 and tick-sample marker=3. Queue slots additionally
carry a transient submission timestamp for latency measurement; this is not
part of the persisted Event ABI.

The Shanghai exchange ChannelNo is decoded from tick bytes 17..18, not from
reserved offset 4 or the framework source ID 89. Supported channels are 1..6.
For each channel independently, a newly discovered stock goes to its least
populated shard. Ties use total stocks across all channels, then shard ID.
Existing stocks never migrate within the run. Balancing measures stock count,
not message rate; a stock generating unusually heavy traffic can still skew
work. A stock appearing under a different channel is an explicit error.

Snapshots carry no exchange ChannelNo. They use the stock's remembered route.
Before discovery, snapshots are journaled with channel 0 and unassigned shard,
and retained in a bounded buffer. When the first tick establishes a route,
buffered snapshots are delivered before that stock's tick. They are not
arbitrarily assigned or reduced to the latest snapshot. Stocks that remain
unmapped are counted at shutdown and computation is not reported healthy.

Tick duplicates are suppressed using a bounded per-channel sequence window.
Unrecognized older ticks are counted as late and invalidate computation;
their event flag preserves that status in replay. The prototype does not
attempt gap recovery/reordering. Snapshot duplicates
use per-stock sequence tracking. This is not proof of full-day feed continuity.

## CPU Placement

The runner acquires distinct L3 domains for all receive workers, the dispatcher,
the journal worker, and each compute shard. With two feeds and eight shards,
that is 12 domains. Idle runner/coordinator work shares an already chosen CPU.
Optional `channels[].cpu` and `cpus.dispatcher`, `cpus.journal`, `cpus.shards`
provide explicit assignments; omitted values select available domains.
The existing SSE lease planner checks conflicts and rejects shortages.
System-wide IRQ placement and unrelated processes are outside this runner.

## Persistence and Replay

The Shanghai adapter reuses the existing segmented journal implementation
without modifying its Shenzhen code. Source ID is 89; each payload is a whole
versioned Event. The existing CRC, record commit trailer, and segment headers
are retained. Segments are preallocated (default 1 GiB); rotation and flush
execute only in the journal worker. No CSV copy is generated.

`pipeline_manifest.json` records the day, version, shard count and CPU plan.
Each journaled mapped event records its actual shard. Offline replay restores
those assignments and requires the original shard count; it does not perform
a new balancing pass or write a second journal. A live capture requires a new
journal epoch and refuses to append to an existing session.
The offline reader requires a cleanly closed journal and rejects invalid
continuity, incomplete records and CRC failures.

```bash
cmake --build BUILD --target sse_event_pipeline
BUILD/sse_event_pipeline sse-t0/config/sse_event_pipeline.example.json
BUILD/sse_event_pipeline SAME_CONFIG.json --replay
```

Set the current trading day, fresh output directory and provisioned feed
addresses before capture. `duration_ms=0` runs until interrupted. Final reports
are `metrics.json` and `replay_metrics.json`, including per-channel stock
distribution, stock ownership, book updates, queue watermarks and failures.

## Models

Add a `model` object with `tick_model` (SSEMODL1 artifact) and `static_json`
(exact-day static metadata). `tick_start_us` defaults to 09:15, matching the
old launcher. Each shard owns an independent engine and recurrent state per
stock. Shanghai units, factors, snapshot auxiliary fields, sample gate and
native model runtime were absorbed from `/home/zane/sse/src` on 2026-09-07.
The gate requires at least 100 traded shares in the quiet batch and either
turnover greater than HistoryAmount/8000 or a mid-price change. Tick output
selection begins at 09:35. Model/gate failures are counted separately from
queue and persistence failures.

For `snapshot_enabled=true`, supply `snapshot_baseline`,
`snapshot_baseline_scaler`, `snapshot_auction`, `snapshot_auction_scaler` and
optionally `auction_csv`. The ensemble runs in [09:30,09:35) with independent
hidden states. Missing Auction59 values are mean-imputed by the existing
scaler and counted explicitly; this is not complete auction validation.
Model statistics are collected in memory and written only after shutdown;
there is no per-prediction CSV write on the compute threads. Model/config
paths must match the recorded manifest during replay.

`SSE_MODEL_MARCH_NATIVE=ON` enables target-host instructions for the Shanghai
tick model target only. Existing Shenzhen compiler settings are unaffected.

## Historical Load Benchmark

```bash
BUILD/sse_event_pipeline BENCHMARK_CONFIG.json --benchmark
```

The `history` object specifies fixed-record input `files`, `start_realtime_ns`,
`measure_start_realtime_ns`, `stop_realtime_ns` and measured playback `speed`.
Input ranges are read and merged into memory before timing. During historical
warmup only, the producer and dispatcher may wait to preserve all events;
a drain barrier completes that work before measured injection begins.
Measured/live fanout remains nonblocking. Source timestamps and sampling
semantics are preserved even when injection speed changes.

Reports include injection scheduling lag, dispatcher ingress latency, per-shard
service/complete latency and tick-marker completion latency. The latter also
includes gate-only markers that do not generate a prediction, and the 100us
sampling policy. Histograms use upper-bound quantized percentiles (<6.25%
relative bucket width); shard percentiles are reported individually, not
averaged into a misleading global percentile. Model factor/infer histograms
include warmup, while pipeline latency histograms respect the measured window.

The benchmark is a memory-to-model/persistence load test. It does not measure
NIC IRQ, UDP socket/decode time, or establish the highest load over a full day.
See `SSE_MODEL_BENCHMARK_20260907.md` for the measured opening-window baseline.
TD integration and journal-to-running-memory recovery handoff remain later
stages. Missing morning data cannot be reconstructed from an afternoon capture.
