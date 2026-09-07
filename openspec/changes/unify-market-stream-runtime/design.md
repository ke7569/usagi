## Context

The repository contains uncommitted user changes and two fixed review snapshots. A first common `MarketDataStream` implementation and unified configuration tooling already pass local tests, but deployed strategy/MD entry points are not yet bound to the new stream. The toolchain is C++11/GCC 4.8.5, CMake 3 and Python 3.6. No live feed is available.

## Goals / Non-Goals

Goals: burst-tolerant ingress, exact recorded-event replay, explicit recording-failure policy, independent market processing builds, real market adapter integration and documented parity evidence through both markets.

Non-goals: changing strategy thresholds/model formulas, a new broker SDK implementation, real orders, production replacement, final Git merge, universal exchange-event abstraction or claiming full-day parity from synthetic tests.

## Decisions

The user's five rules on 2026-09-06 supersede all prior AI-generated SSE sampling baselines: quiet is per instrument and strictly greater than 100us; sample on turnover >= HistoryAmount/8000 OR elapsed exchange time >=100s OR mid-price change with at least 100 traded shares; at most one sample per instrument/exchange timestamp; initialize from the first valid two-sided book update at/after 09:30 without an extra sample, never a 09:25 seed. Keep one indexed deadline per instrument, drain due deadlines before new book mutation, and preserve identical recorded-clock observations in live/replay. Old global sampling is removed, not retained behind a mode flag. Historical reference copies are evidence, not active implementations.

1. One receive producer uses nonblocking `recvmmsg`, one serialized application callback consumes a preallocated SPSC queue, and one optional writer consumes a second queue. Neither disk sync nor JSON formatting belongs on the receive thread. Receive batches are syscall boundaries, never model sample boundaries.
2. Record full payload, channel/source identity, stream order, monotonic receive time, separately identified realtime timestamp and explicit idle events. Replay validates all segments before callbacks and uses recorded time with no artificial EOF flush. Models consume their existing market-specific representations after decoding.
3. Required recording remains the conservative default. An explicit optional mode degrades recording on write/queue failure, exposes a sticky error and continues complete market-data processing. Transport truncation/overflow is always fatal. Expose distinct health information; transport permission is only a necessary condition for new risk, not a complete trading gate. No automatic flattening or disabling of cancellation/report handling.
4. Import only needed source dependencies from fixed snapshots with provenance; integrate shared-file differences deliberately. Keep independent market state and processing targets. Existing deployment/SHM/SDK interfaces remain compatibility paths until verification permits replacement.
5. Validate actual decoding/book/sampling/factor/model code with deterministic fixtures and capture/replay comparison. A missing real model or recording is a documented verification gap, not a reason to fabricate parity. Do not extend an offline-only implementation that live does not call.

## Risks / Trade-offs

- Asynchronous durability has a tail-loss window; the sync interval is not a maximum loss guarantee when queues or disks stall. Report written and durable watermarks separately.
- A 1000-packet loopback test is not a NIC or full-model latency benchmark. Preserve receive timestamps and report the conditions of all performance checks.
- Receive-time semantics can change historical sampling. Test strict threshold boundaries and compare old behavior separately from intended bug fixes.
- Existing raw SZE recovery and SHM ABI cannot be silently replaced by a new recording container. Preserve their contracts and distinguish recording integrity from market sequence recovery.
- Full-application parity includes TD events and scheduling beyond MD idle events. This phase does not claim those are reproduced.

## Migration Plan

Finish transport policy and tests; establish market build boundaries; connect SSE then SZE processing; run local regression and publish an execution record with remaining deployment gates. No production files are replaced. The old entry points remain available during incremental integration.

The strategy increment reuses the existing ZStrategy decision implementation through a narrow execution/clock interface. StrategyBase constructs a framework adapter; normal strategy formulas are unchanged, while nonfinite signals and unsafe quantity conversions are guarded. A Shanghai session owns per-instrument strategy state and same-sample book views, combines processing/recording/session/account readiness into a new-order gate, and retains owned cancellation/report handling after that gate closes. The standalone capture/replay entry supports an explicitly selected paper-intents executor, never TD. Event time drives paper cancellation deadlines with no EOF flush or fabricated fills.

Inspection of the installed IWCStrategy ABI shows that stop/terminate/block are nonvirtual. A thin subclass owning an independent raw-stream thread cannot safely override the framework stop sequence. Therefore the legacy SSE get_obj remains closed until a unified host explicitly owns stream/strategy/TD lifecycle; it is not marked migrated just because the standalone entry works.

The supported migration entry is a versioned, opaque C runtime API with explicit create/start/request_stop/join/destroy operations, used by a project-owned launcher. Read-only inspection of the installed main shows that it calls get_obj but does not subsequently start the strategy, and its control-center start/stop methods are empty. Thus the new runtime must not be cast to IWCStrategy or silently started by the old factory. Legacy get_obj remains an explicit rejection with migration guidance; the new entry is separately loadable/testable for both markets. The same application assembly is used by the command line and shared-library caller, without vendor dependencies.

Account readiness is a separate epoch-bound, quiescent snapshot protocol. It requires matching account/source/day, complete expected positions, explicit completion of an empty open-order query, finite cash and connected state. Activity during collection invalidates the snapshot; stale epochs/tokens cannot complete a newer one. Paper explicitly supplies its configuration baseline through this protocol, labeled simulation. A real adapter without complete query markers cannot claim readiness. Nonempty startup orders are rejected until an order-ledger reconciliation implementation exists; no fabricated fills or auto-cancellation is introduced.

SzeRecoveryDriver binds selected .szej records and read-only ReplayHandoffConsumer to the same normalized SZE processing implementation. Sparse original wire sequences are valid after filtering; contiguous selected event IDs are checked instead. Journal analysis may explicitly use exchange time, but live handoff requires a complete same-boot receive-time anchor and explicit nonzero generation, with source/day/continuity/liveness validation before readiness. Disk and SHM formats are not changed. Its unified trading-host/config binding remains separate work.

The current paper-stage execution boundary retains request ownership for late reports, with a fail-closed 100000-request session cap and a bounded cancel schedule. It is not a production order ledger. Broker-specific terminal/fill ordering, reconciliation and bounded retirement must be implemented before a real TD executor is enabled; deleting ownership on the first terminal report could lose a later fill.

The user requested a later rename to usagi with common/, sze/ and sse/ ownership directories. That is deferred until strategy/protection/recovery verification and recorded in the refactor plan; this increment does not move paths or rename exchange/source identifiers.

Subsequent source relocation is recorded separately in [migrate-usagi-layout](../migrate-usagi-layout/design.md). Use its file-map.json to resolve the historical source/document paths in this change. The relocation does not complete this change's outstanding legacy-host integration or live acceptance.

## Open Questions

SSE sampling semantics are now decided by the user. Actual feed distribution, throughput targets, CPU allocation, complete model/data availability and broker SDK validation remain environment-specific acceptance inputs. They do not authorize changing strategy semantics or inventing exchange recovery rules.
