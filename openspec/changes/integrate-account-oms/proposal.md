## Why

Order accounting currently lives in ZStrategy while TD maintains separate transport routes and cancellation is delegated directly to backends. The handoff in `/home/external/docs/OMS_INTEGRATION_AGENT_HANDOFF.md` requires one account-level authority for reservations, reports, cancellation and recovery, actually used by both market runtimes.

## What Changes

- Introduce a portable, in-process account OMS and normalized TD contract. TD remains the protocol/connection adapter; strategy retains signal and sizing rules.
- Register identity and reserve risk before dispatch, serialize state updates, handle reentrant/early callbacks and retain uncertain sends.
- Implement per-order dual-stream accounting, bounded cancel management, account cash/share/rate/self-trade checks, active-order reconciliation and durable intent/audit recovery.
- Wire shared OMS execution into SZE/SSE sessions, with unchanged non-filling Paper behavior and a separate scripted report backend for deterministic integration tests.
- Adapt the available ATP boundary without inventing SDK semantics. Unsupported legacy sends/fast paths and incomplete reconciliation stay explicitly closed.
- **BREAKING**: strategy-intent profiles require explicit OMS simulation budgets; unmanaged strategy/TD execution is no longer a permitted bypass. No production deployment or real broker connection is authorized.

## Capabilities

### New Capabilities

- `account-oms`: Single-owner order/account state, execution backends, recovery/audit and runtime integration.

### Modified Capabilities

None in the main specification tree. Existing migration and market-processing behavior remains the baseline; expected execution/risk corrections are specified in this change.

## Impact

`common/oms`, existing execution/reconciliation helpers, ZStrategy and StrategySession, both market runtime adapters, application/config assembly, ATP/Deepwin boundaries, CMake and offline tests. Market decoding, sampling, factor formulas and model arithmetic are not changed. Original OrderMgrServer is a reference only, not a new runtime dependency.
