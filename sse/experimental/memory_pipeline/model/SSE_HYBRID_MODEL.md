# SSE Snapshot-to-Tick model handover

The SSE model service now has two independent model families:

- **Snapshot**: the 36-factor baseline and 95-factor Auction59 GRUs from the
  2026-08-17 handoff;
- **Tick**: the v0.4 `v04-legacy-midmix-sse` 50-factor GRU, sampled only at
  CompleteOrderBookSH L2 batch ends with the strict next-local gap `>100us`.

Both families generate predictions concurrently on their own accepted event
streams. Their hidden states are independent and must be reset per instrument
and trading day. The selected output is deliberately hard-switched by SSE
exchange event time:

| Event time | Selected output |
|---|---|
| `[09:30:00,09:35:00)` | Snapshot ensemble |
| `09:35:00` and later | Tick model |

The tick model is still advanced before 09:35, so the first post-switch tick
prediction uses its continuous warm state. A tick prediction generated before
09:35 is retained for diagnostics but is not selected. Snapshot rows after
09:35 are rejected because the Snapshot handoff's valid window is half-open.

The orchestrator is `sse_hybrid_model.*`. It exposes separate `on_tick` and
`on_snapshot` calls because the two feeds have different event cadence. Each
call reports whether its model generated a prediction and whether that source
was selected. A failure of the selected model rejects the output; the other
model is never silently substituted.

The production strategy is still not wired to this API: vendor decoding,
factor generation, and instrument/day state ownership remain target-host
integration work. Live batch-end detection and the standard sample gate are
available in `sse/experimental/memory_pipeline/market_data/sse_batch_end_sampler.*`; their output feeds
the frozen 50-factor adapter before `on_tick` is called.
