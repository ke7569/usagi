## Context

The local source-layout migration is complete; portable SZE/SSE runtimes passed 34 CTest targets, while real TD reconciliation and legacy SSE host lifecycle remain unaccepted. Existing ZStrategy owns fill/pending counters, ProtectedExecution registers IDs after sending, and PaperExecution owns cancel timers. ATP owns transport routes but lacks an epoch-bound complete snapshot contract.

The implementation baseline is retained under `/home/ref/usagi-oms-baseline-20260906-Egyu7w`; work continues on `feature/oms-integration` without committing prior user changes. The external OrderMgrServer is a scenario reference, not a linked dependency.

## Goals / Non-Goals

Goals: one account/order authority, real runtime wiring for both markets, deterministic backends, active-order recovery, bounded resources and explainable durability/fault handling.

Non-goals: changing model/sampling formulas, adding a distributed service, native FAK where the gateway does not support it, faking missing query semantics, connecting a production account or switching deployed binaries.

## Decisions

### Ownership and calls

```text
SZE/SSE processor -> shared StrategySession/ZStrategy -> OMS strategy client
                                                    -> account admission/reservation/ID
                                                    -> TD backend send/cancel/query
TD normalized events -> same OMS ledger -> read-only strategy position view
```

The in-process `common/oms` owns all live order quantities, reservations, account cash/sellable shares, self-trade exposure and cancellation state. ZStrategy keeps decision parameters and reads snapshots; its old Order/Trade maps and incremental bookkeeping are removed. ProtectedExecution becomes a gate/identity facade, not another request registry. AccountReconciliation remains a legacy validation helper and regression target; OMS extends the explicit completion/epoch contract for active orders without maintaining a second account ledger. Public OMS types contain no Deepwin/SDK structures; legacy field conversion belongs at the execution/TD boundary.

Each OMS owns one canonical broker/funding account. Multiple clients/markets can share that instance. Non-simulated accounts require exclusive local ownership, a fixed host-local account lock and durable journal; uncoordinated multi-host ownership is rejected. Separate CLI paper instances remain explicitly simulated, not a shared real budget. There is no new network hop on the order path.

### Ordering, identity and failures

Internal IDs, owner/intent IDs and reservations are registered before a backend sees a command. Account mutations are mutex-serialized. Backend calls occur outside the state mutex through one bounded dispatch queue; callback handling can therefore complete synchronously or on another thread. The dispatcher serializes SDK calls and rechecks stop/health immediately before dispatch. Callback sinks hold weak instance ownership and immutable connection scope; stale day/epoch/gateway/account events cannot affect the new instance.

Orders distinguish queued/not-sent, submitted, send-unknown, accepted, partial, cancel-pending, canceled/rejected/filled and reconciliation-required states. An exception/ambiguous send is not an ordinary rejection. Early broker-ID-only reports use a bounded, expiring quarantine until an exact mapping exists; no symbol-based adoption.

### Quantity, money and cancellation

Every order owns its remaining reservation. Cumulative regressions never replace the accepted high-water mark or release another order. Deduplicated Trade facts use stable execution IDs and, where available, cumulative watermarks. Taking the maximum of counters requires a declared complete-from-origin trade stream; otherwise uncertain overlap is observable and closes new risk until reconciliation. Late fills change that order's facts and account position without a second terminal release; contradictory terminal facts require reconciliation.

Prices/money use checked fixed-point integer units (1/10000 CNY), quantities are shares. Fill quantity, priced quantity, execution amount and fees are separate. Missing prices/fees are never replaced by an invented execution price or zero fee: unknown buy cost and configured fee bounds remain reserved, and unknown sell proceeds are not spendable.

Cancel intent survives missing broker ID, rate limiting and transient failure. One bounded timer per order coalesces requests, checks state/ownership on every attempt and limits retries/timeouts. Native FAK and limit-then-cancel are distinct capabilities; the existing strategy's 1001ms emulation starts at submission, not receipt of the broker ID. No EOF/destructor clock advance is introduced.

### Recovery and persistence

Recovery stages explicit successful cash, position, order and required trade completions under one epoch/token. Any intervening activity invalidates the snapshot. A complete snapshot can restore known active orders/reservations and read-only external orders; external orders affect risk but are not automatically cancelable. Snapshot cash and sellable quantities have explicit free-after-working-reservations semantics, avoiding double reservation. Unresolved unknown sends are never blindly resent or inferred absent from an open-orders-only query.

Durable mode appends a length/sequence/checksum-framed journal and synchronizes the intent before any real send. Recorded normalized events support reconstruction without replaying outbound side effects. A restarted instance always requires a fresh snapshot; old monotonic deadlines are not reused across boots. Corrupt/truncated tails are preserved and block new risk, not silently repaired. Audit failure stops new orders but retains known-order cancellation and visible in-memory reports. Simulation may explicitly use memory-only audit; its results do not claim crash durability.

### Supported backends and legacy paths

Paper continues to record commands only, never manufactures fills or cancel acknowledgements. A separate scripted backend injects normalized replies/faults and is wired into the same market application path. Paper profiles must supply an explicit simulation cash budget; this intentionally replaces the previous zero-cash protocol placeholder.

ATP integration retains wire ID mapping and raw error provenance in the adapter. Its normal/direct sends must route through OMS authorization, or be explicitly rejected; old framework strategies cannot keep an unmanaged fast path. Actual query watermark/completion and reconnection guarantees are not inferred from isLast. A channel lacking them stays restricted even when its adapter compiles. Guojun protocol choice remains blocked on the actual account/SDK contract, not guessed from the reference package.

## Risks / Trade-offs

- Durable pre-send synchronization adds measurable latency; benchmark it separately from memory mode and do not call asynchronous logs durable.
- Terminal orders/trade IDs remain bounded and retained for late reports; exhausting capacity disables new risk rather than recycling ambiguous identities.
- Legacy strategy/TD gating is an intentional safety change; existing deployments are not touched, and operators must not replace libraries without reviewing the support matrix.
- Fixed fixtures prove state and integration behavior, not live NIC tail latency, model quality or real broker acceptance.

## Migration Plan

Implement contracts/journal and ledger scenarios first, then cancellation/risk/recovery, then replace strategy state and wire both runtimes/backends. Validate portable and SDK builds, replay/fault cases, isolation and measured latency; keep support status and operational requirements in one OMS contract document.

## Latency Attribution Follow-up (2026-09-07)

The original benchmark included intent construction and post-send work. Add a separate diagnostic core compiled from the same sources with fixed-storage, thread-local timing scopes. Normal production targets compile the probes out. Compare identical preconstructed intents with total-only, coarse backend-boundary and detailed-stage measurements; retain the legacy mixed total only as historical evidence. Log inclusive/exclusive scope costs and probe overhead explicitly, never add percentile components as if they belonged to one order. Disk synchronization, risk rules and dispatch ordering remain unchanged in this measurement-only step.

## Logging Optimization Follow-up

Hot events use a versioned compact binary payload inside the existing CRC-framed journal. Old JSON frames remain readable; replay converts binary records to the existing state-machine inputs on the cold path. Memory-only simulation retains sequence counters and bounded audit events, without formatting discarded JSON. A single bounded background writer owns disk IO; ordinary append calls never wait for disk. Explicit durable intents/epoch/snapshot barriers still wait for confirmed persistence, not an asynchronous enqueue, so this change does not silently weaken crash recovery. Queue exhaustion or worker failure closes new risk and remains observable. Risk rules and ledger mutation order are unchanged.
