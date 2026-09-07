## 1. Baseline and Contracts

- [x] 1.1 Read the handoff/reference and current call chain, preserve the dirty baseline, and rerun the existing 34 CTest targets.
- [x] 1.2 Implement normalized OMS/TD identities, capabilities, error/report and risk/state contracts without SDK dependencies.

## 2. Order and Account Core

- [x] 2.1 Implement pre-dispatch registration, safe callback serialization, per-order accounting, explicit unknown money/quantity and bounded ownership.
- [x] 2.2 Implement atomic account reservations, price/lot/quantity/notional/capacity/rate checks and account-wide self-trade protection.
- [x] 2.3 Implement retained/coalesced cancel intents, missing-ID handling, capability-specific FAK, retries, deadlines and stop behavior.

## 3. Recovery and Audit

- [x] 3.1 Implement exclusive ownership and framed durable intent/audit storage with corrupt-tail/failure handling and bounded replay.
- [x] 3.2 Integrate explicit query completeness and quiescence with active/unknown/external order recovery; reject stale epochs and unsafe account sharing.

## 4. Runtime and TD Integration

- [x] 4.1 Replace strategy-owned fill/pending state and the duplicate execution registry/timers with OMS views and clients; wire both market runtime entries.
- [x] 4.2 Preserve non-filling Paper semantics, add a scripted fault backend, and bind explicit OMS budgets/capabilities through the common configuration flow.
- [x] 4.3 Integrate the available ATP transport/report boundary and close unmanaged ordinary/direct/legacy sends; document concrete query/SDK limitations.

## 5. Acceptance

- [x] 5.1 Exercise the handoff accounting/cancel/risk/recovery/concurrency/crash/capacity scenarios against actual implementations and both market entry points.
- [x] 5.2 Run affected portable/SDK/config regressions and a bounded offline latency/resource comparison; distinguish expected execution fixes from unchanged market/model outputs.
- [x] 5.3 Update one OMS operating contract/support matrix and concise implementation evidence, with no production connection, deployment or automatic trading enablement.

## 6. Latency Attribution

- [x] 6.1 Add compile-time-isolated stage probes and matched total/coarse/detailed benchmarks without changing trading or durability semantics.
- [x] 6.2 Verify timing boundaries, accounting equivalence, disabled-probe isolation and existing regressions; run repeated controlled offline measurements.
- [x] 6.3 Record measured pre-backend/backend/post-backend and internal costs, instrumentation overhead, workload limits and evidence-based optimization priorities.

## 7. Compact Asynchronous Logging

- [x] 7.1 Replace hot-path JSON records with compact versioned events, preserve legacy replay and skip unused memory-mode serialization.
- [x] 7.2 Add bounded background journal writes with explicit durability barriers, fault propagation, drain/stop and queue observability.
- [ ] 7.3 Verify recovery, ordering, saturation and latency against the same workload; document the remaining pre-send durability cost without changing risk checks.
