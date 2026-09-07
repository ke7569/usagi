# Shenzhen Production Architecture TODO

## Snapshot fallback

- [ ] Make Snapshot reception an independent raw journal/SHM producer.
- [ ] Feed Snapshot and source-88 candidates into separate queues with explicit ownership.
- [ ] Add candidate freshness, trading-session, and timestamp-alignment checks.
- [ ] Keep the Turnover arbiter deterministic: valid full order book wins when it is current; otherwise use valid Snapshot; otherwise suppress trading.
- [ ] Record the selected source, both Turnover values, candidate ages, and rejection reason for every trading decision.

## Throughput and reliability

- [ ] Confirm NIC IRQ/RSS affinity does not share the receive, flush, prediction, or TD CPUs.
- [ ] Measure journal publish-to-flush backlog, flush latency, segment rollover latency, and disk headroom.
- [ ] Benchmark Snapshot and source-88 processing at the target universe size, including model inference time per symbol.
- [ ] Keep capture independent from recovery and prediction; recovery failure must not stop market-data capture.
- [ ] Add per-shard health, stale-symbol, malformed-record, and sequence-gap metrics instead of failing the whole universe.

## Configuration and scheduling

- [ ] Synchronize the latest research configuration before each trading day and validate hashes/schema before installation.
- [ ] Preserve the canonical layout under `/home/zane/configs`; generated main configs must use absolute canonical paths.
- [ ] Generate recovery shards from explicit `ins_params[*].cpu` assignments when present, with distinct state CPUs.
- [ ] Make the daily capture/recovery/stop launcher resolve the current date dynamically and fail clearly when a required artifact is absent.
- [ ] Remove or disable expired date-specific timers after the generic daily timers are validated.
- [ ] Add a pre-open readiness report covering config date, model hash, CPU map, journal path, SHM path, and active services.

## Trading safety

- [ ] Keep Snapshot fallback and full-orderbook prediction state separate; never transfer hidden state between models.
- [ ] Enforce order limits, stale prediction rejection, position checks, and duplicate-order suppression at the TD boundary.
- [ ] Keep `capture_only` and live-routing modes explicitly distinct in config validation and startup logs.
