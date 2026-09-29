# Shanghai Model Integration and Opening-Window Baseline

Measured on 2026-09-07, using the isolated usagi checkout on 10.49.129.190.
The model integration works; the latency baseline does not meet the intended
low-latency behavior. Production was not switched.

## Method

- Historical source: 2026-09-04 primary tick and snapshot records.
- Warmup: from 09:14 to 09:30, losslessly processed before measurement.
- Measured interval: 09:30:00–09:31:00 at original speed, all available stocks.
- Coverage: 6 exchange channels, 2,316 stocks; channel-local shard populations
  differ by at most one stock, stable for the run and replay.
- Total raw events: 5,607,459; measured raw events: 2,829,109.
- Highest one-second raw-event count inside this interval: 54,305/s.
- Eight compute shards plus input, dispatcher and journal, each on a distinct
  L3 domain. These are offline input roles, not a live NIC test.
- Real SSEMODL1 weights and staged 20260904 static configuration; the dual
  run also loads the existing baseline/Auction59 snapshot models.
- No Auction59 sidecar was supplied; 45,692 snapshot predictions were marked
  as missing auction inputs and used the existing mean-imputation behavior.

## Results

| Measurement | Tick + snapshot ensemble | Tick only; snapshot auxiliary retained |
|---|---:|---:|
| Input events accepted | 5,607,459 | 5,607,459 |
| Journal records including sample markers | 6,961,654 | 6,961,654 |
| Ingress / shard overflows | 0 / 0 | 0 / 0 |
| Journal complete | Yes | Yes |
| Tick predictions | 133,315 | 133,315 |
| Snapshot predictions | 45,692 | 0 |
| Model runtime errors | 0 | 0 |
| Rejected book updates | 612 | 612 |
| Input scheduling lag p99 | 2.49 ms | 1.77 ms |
| Dispatcher ingress latency p99 | 14.68 ms | 15.20 ms |
| Per-shard end-to-end p99 range | 18.87–20.97 ms | 18.87–22.02 ms |
| Per-shard sample-marker completion p99 range | 26.21–27.26 ms | 27.26–28.31 ms |

The 612 rejected book updates are unknown-order deletes. They keep the
computation-health flag false; their cause has not been established by this
latency test. Model runtime success does not establish complete book state.

For an example dual-model shard, tick inference p50/p99 was approximately
70/111us, factor generation 70/311us, and snapshot ensemble inference
1.31/1.70ms. These model-stage histograms include warmup. Disabling snapshot
inference did not eliminate the millisecond-scale delay: the ingress/dispatcher
path and burst scheduling need investigation before further compute sharding
or a production cutover can be justified. Injection scheduling contributes to
these end-to-end measurements and must not be mistaken for pure compute time.

An initial 10x warmup run lost ingress events and invalidated its journal.
That run was excluded from the table; the final runner uses lossless warmup
and a barrier before measurement. A dispatcher stop race discovered by
regression tests was also fixed so the last submitted event is drained.

## Replay and Artifacts

Replayed all 6,961,654 dual-model journal records. Channel/shard assignments,
per-stock prediction counts, per-shard quality counts and prediction sums
matched exactly (sum difference zero in all eight shards). This comparison
checks aggregate reproducibility, not a separately exported row-by-row golden
prediction file. Real-weight unit tests also compare the consumer with the
underlying engine and test per-stock recurrent-state independence.

Server outputs:

- `/home/zane/usagi-experiments/model_peak_20260904_1x_warm/metrics.json`
- `/home/zane/usagi-experiments/model_peak_20260904_1x_warm/replay_metrics.json`
- `/home/zane/usagi-experiments/model_peak_20260904_tick_1x/metrics.json`

Configurations live in `sse/experimental/memory_pipeline/config/benchmark_sse_20260904*.json`; reruns
must select new journal directories. The files are source-89 journals using
the existing segment/CRC format, not CSV intermediates for live computation.

Next performance work should isolate dispatcher processing time from input
scheduling lag, inspect timer/map/allocator costs under bursts, and repeat
the same dataset after changes. Live NIC latency and full-day peak coverage
remain separate acceptance steps.
