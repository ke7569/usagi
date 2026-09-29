# SSE/T0 Runtime Split

The two-day rollout should keep three boundaries independent:

```text
UDP multicast channels
        |
        v
shared UDP channel runtime (`sse/experimental/memory_pipeline/udp_runtime/UdpChannelRuntime.*`)
        +----------------------------> raw capture / diagnostics
        |
        v
exchange decoder (SSE or SZE) ------> normalized L2 event stream
        |
        v
factor engine (CompleteOrderBookSH for SSE)
        |
        v
strategy process (HStrategy or ZStrategy)
```

The socket/channel runtime can be shared by SSE and SZE because both feeds are
multi-channel UDP streams. The decoder must remain exchange-specific: packet
headers, channel sequence semantics, order/trade identifiers, cancel rules, and
batch-end sampling differ. The strategy process must consume normalized events
and must not own multicast sockets. The existing SZE receiver implementations
are the first migration target; this offline phase does not change their
production behavior.

SSE and SZE factors are separate contracts. SSE v0.4 consumes the ordered 50
factors in `sse/experimental/memory_pipeline/model/factors.txt`; SZE continues to use its own factor
engine/model. The old generic `SsePredictor` path is not the SSE v0.4 runtime.

Within SSE, Snapshot and tick factors are also separate contracts and event
streams. Opening Snapshot rows feed the 36/95 dimensional dual-GRU ensemble;
strict `>100us` CompleteOrderBookSH batch-end rows feed the 50-factor tick GRU.
Both advance independent per-stock/day hidden states. The output selector uses
exchange event time: Snapshot for `[09:30,09:35)`, tick from exactly 09:35.
The selector does not transform factors or provide cross-model fallback.

The live `>100us` rule is implemented by
`sse/experimental/memory_pipeline/market_data/sse_batch_end_sampler.*` as a trailing-edge one-shot state
machine. Every normalized SSE update rearms an absolute `CLOCK_MONOTONIC`
deadline; a batch closes only after a strictly greater gap. The two-phase API
closes an expired old batch before the next event mutates the reconstructed
book, so timer scheduling latency cannot mix two batches. Only the final
CompleteOrderBookSH candidate per dirty instrument is emitted, and the normal
turnover/100-second/mid-change gate still applies before factor generation.
Sequence gaps discard the pending batch and require explicit recovery.

## Rollout boundary

- Tomorrow: run the raw observer and decoder in offline/loopback mode, then move
  the same capture binary and channel config to the target host. Capture first;
  no strategy or order routing is required.
- The day after: run the market-data process for the full session, persist
  normalized SSE events and factor rows, and run the 50-factor model skeleton
  with one hidden state per stock-day.
- Only after a full-day factor report is stable should `HStrategy` be connected
  to the strategy process. TD credentials and order routing remain outside the
  market-data process.

## Why not one exchange-specific script?

Use one common launcher/runtime for socket lifecycle, channel metrics, capture,
rotation, and shutdown. Pass an exchange decoder plugin and a channel manifest
to it. This avoids duplicating operational code while preserving a hard protocol
boundary where SSE and SZE behavior can be tested independently.
