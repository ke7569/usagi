# Shenzhen health domains and direct sharded ring prototype

## Safety and compatibility

The existing `ShmRingHeader` remains format version 1 and its fields remain in
place for old readers. New health state is published in separate mmap files.
The direct sharded ring is disabled by default and does not replace the global
ring. This branch is a prototype and must pass production-host A/B testing
before activation.

## Ownership

State files have one writer and any number of read-only observers:

- `<ring>.health`: capture owns feed, journal, and global-ring health.
- `<ring>.shard.<id>.health`: recovery shard `id` owns its readiness, lag,
  overruns, and the book/prediction records for its symbols.
- `<ring>.td.health`: TD owns login, account, position, and callback health.
- `sze_health_status` is a read-only aggregator. No shard writes a global
  readiness value in the new ABI.

The legacy readiness field remains only as a compatibility view. New trading
gates must use the per-shard state page.

## Failure scope

A feed gap with no channel or symbol attribution has `global_feed` scope and
blocks all symbols. A shard ring overrun blocks that shard. A deterministic
book invariant failure blocks only its symbol. Journal degradation does not
change feed continuity: it permits risk reduction but blocks new risk by
default because a restart can no longer reconstruct the online state.

`can_trade(symbol)` requires healthy feed and ring domains, a live owner shard,
a valid symbol book, healthy prediction, and ready TD. Durable journal health
is additionally required for new risk under the default policy.

## Direct sharded ring

The prototype routes in the capture thread after one wire decode:

```text
decode -> numeric symbol lookup -> exactly one shard ring publish
```

There is no dispatcher thread or ingress ring. The symbol-to-shard table is a
preallocated 1,000,000-entry byte array indexed by the six-digit symbol. The
event stores the global event id and feed sequence; each shard slot also has a
contiguous shard event id. A symbol has one immutable shard assignment.

The optional capture configuration is:

```json
"direct_sharded_ring": {
  "enabled": false,
  "shard_count": 8,
  "capacity_per_shard": 262144,
  "max_payload_bytes": 256,
  "paths": ["/dev/shm/sze.shard.0.events"],
  "assignments": {"000001": 0}
}
```

When enabled in the prototype, capture mirrors each selected event to its one
target shard ring while retaining the production global ring. Trade must not
consume raw shard rings. A later production switch requires recovery consumers
to use one SPSC shard ring each and must be a separate reviewed deployment.

## Benchmark

Run:

```bash
./sze_sharded_ring_benchmark --events 1000000 \
  --peak-events-per-second 200000 --enforce
```

The output is explicitly marked `synthetic=1` and reports publish, consume,
and receive-to-consume p50/p95/p99/p99.9/max, throughput, and overruns. Gates
are incremental p99 <= 1 us, p99.9 <= 3 us, and throughput >= 1.5 times the
declared peak with zero overruns. Synthetic results do not represent live NIC
latency; production activation still requires capture-host A/B observation.
