## 1. Shared Stream

- [x] 1.1 Implement and verify full-payload burst recording, idle-event replay and input integrity validation (prior increment: 4 CTest targets passed).
- [x] 1.2 Separate required/optional recording failures from input failures and test both policies (13 raw CLI cases plus C++ overflow/preflight checks).
- [x] 1.3 Document stream format, configuration, stop semantics, durability and operational limits (`docs/market_data_stream.md`, loopback example profile).

## 2. Build Boundaries

- [x] 2.1 Record required source provenance and integration differences from the fixed SZE/SSE snapshots (`docs/market_stream_source_provenance.md`).
- [x] 2.2 Establish independently buildable market processing targets without cross-market model dependencies (both binaries built; link lines, nm and ldd checked).

## 3. Shanghai Integration

- [x] 3.1 Connect the common standalone driver to real SSE decoding, order book, sampling, factor and prediction processing; old strategy plugin remains disabled.
- [x] 3.2 Verify SSE live/replay outputs and strict sampling/failure boundaries with deterministic fixtures and synthetic model weights, not production checkpoints.
- [ ] 3.3 Migrate the existing SSE framework strategy entry, including safe construction/stop. The standalone entry is connected; legacy get_obj remains closed because IWCStrategy stop/terminate/block are nonvirtual and do not own the raw stream thread.
- [x] 3.9 Provide a versioned owned-runtime C API and launcher sharing market application assembly, with stop-before-start, repeated stop/join, failure and destruction tests; keep incompatible legacy get_obj explicitly rejected (11 API scenarios, including first/second thread creation failure).
- [x] 3.6 Introduce a reusable execution/clock boundary around existing ZStrategy; preserve normal decision formulas and reject nonfinite/out-of-range numeric conversions. The old SZE Deepwin adapter builds with namespaced framework symbols.
- [x] 3.7 Wire same-sample SSE outputs to per-instrument strategy state and enforce new-order gates while keeping owned cancellation/reports available. The assembled executor is explicitly paper-only, not a live TD adapter.
- [x] 3.8 Verify offline stream-to-order-intent parity with nonzero orders/cancels and synthetic models, without connecting TD. Plugin lifecycle acceptance remains separately open in 3.3.
- [x] 3.4 Replace global SSE quiet batching with bounded per-instrument deadlines, first-opening-book initialization and same-ExTime sample deduplication.
- [x] 3.5 Remove superseded active sampling code/configuration and verify user-rule, replay and unrelated-market regressions (14 CTest targets, 19 processing-profile, 28 unified-config and 12 daily tests passed).

## 4. Shenzhen Integration

- [x] 4.1 Bind existing .szej/SHM replay-to-live to SzeStreamProcessor through SzeRecoveryDriver, preserving disk/SHM layouts and adding source/day/generation/clock/continuity validation. Offline journal mode never grants live readiness.
- [x] 4.3 Bind the SZE recovery driver and shared strategy session to the owned runtime/strict profile. An explicitly labeled complete Paper snapshot precedes admission; recovery-journal stays analysis-only and handoff needs live continuity. Real broker reconciliation is not enabled (6 recovery CLI/API cases; SZE synthetic-model stream-to-intent parity passed).
- [x] 4.4 Share per-instrument ZStrategy ownership and add an epoch/account/day-bound complete-snapshot readiness protocol, preserving market-specific view construction and sampling (13 reconciliation cases and common/SZE/SSE session tests).
- [x] 4.2 Verify SZE live/replay factor/book outputs and regression behavior with deterministic 1000-datagram fixtures; production-model inference parity remains unverified.

## 5. Acceptance

- [x] 5.1 Run the local build/test matrix and update configuration/version-lock integration as needed (29 CTest targets; 28 unified, 27 processing-profile and 12 existing daily tests; independent runtime libraries checked for SDK/cross-market dependencies).
- [x] 5.2 Publish execution evidence, remaining full-day/SDK/live-feed checks and safe next steps without deployment (`docs/market_stream_execution_20260906.md`, historical SSE candidate smoke included).
