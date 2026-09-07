# SSE Market Data Processing

The standalone `SseStreamProcessor` owns the SSE decoder, reconstructed books,
sampling windows, factor state and model state. Both `t0_sse_stream capture`
and `t0_sse_stream replay` invoke this same processor through the common
`MarketDataStream` transport:

```text
UDP channels -> MarketDataStream -> serialized StreamEvent -> SSE processor
recorded .t0md directory -------------------------------> same SSE processor
```

`common/stream/MarketDataStream.*` owns batched UDP receive,
bounded queues, timestamps and asynchronous recording. It neither decodes
SSE/SZE records nor decides when to sample. Borrowed payload bytes remain
valid only until the callback returns. Consumers that need longer ownership
must copy them.

`UdpChannelRuntime.*` is a compatibility wrapper around the serialized stream
callback, with recording disabled. It exposes datagrams but omits recorded
idle events. New market processors consume `StreamEvent` directly so capture
and replay observe the same time progression.

## Current SSE Contract

`sse-per-instrument-v2` uses a separate quiet deadline for each configured
stock, strictly greater than 100 us since its last relevant update. Other
stocks and Snapshot messages advance observed time without rearming that
stock. Global transport idle events supply clock observations, not an SSE
sample boundary. Actual callbacks run when those observations are processed;
the threshold is not a hard wall-clock latency guarantee.

The first book-changing tick at or after 09:30 that creates a valid two-sided
book initializes the stock's window without emitting a sample. Subsequent
quiet cuts use the OR gate: turnover increase `>= HistoryAmount/8000`,
exchange-time increase `>= 100` seconds, or mid-price change `> 1e-6` with
traded volume increase `>= 100` shares. The same exchange timestamp permits
at most one sample, and a rejected duplicate timestamp does not reset the
window. The indexed deadline heap holds at most one entry per configured
stock. See [SSE sampling contract](../contracts/sse-sampling.md) for the exact API and units.

The former global sampling path and archived `PredictionEngine` rules are
superseded, not alternative runtime modes. The immutable reference archive
is retained for source audits. The model factor ABI label is preserved while
the stream contract identifies the corrected sampling behavior.

## Remaining Integration

This standalone target does not replace the existing Deepwin strategy
plugins or connect TD. `MDEngineSZE.cpp` and `MDEngineSZEL1.cpp` retain their
existing adapter paths, and SZE `.szej`/SHM recovery has not moved into
`.t0md` transport. SSE sequence failures remain sticky; complete resync,
strategy entry migration and full-day numerical validation are separate
remaining work. Final regression results for the revised sampling contract
are recorded in `docs/market_stream_execution_20260906.md`.
