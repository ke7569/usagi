# Merge execution-0929 into sze-dev

Inputs:

- sze-dev: 8823a3065ab6d7280ccc05a709b5b0ff4ebfdcea
- execution-0929: ed4cff4d03ae2d2b48ec594975dbda846047e1c7

This is a full history merge, including the Shanghai runtime/OMS work
on which the shared SH/SZ external execution update depends.

Conflict decisions:

- Preserve the Shenzhen current-state snapshot assembler and automatic
  reconciliation/retry path. Use the incoming certified, paginated snapshot
  collector and owner-thread callbacks in stream mode.
- Keep the two ATP query state types, maps and mutexes distinct. A single
  SDK trade-query callback dispatches to the appropriate mode.
- Retain OMS epoch token reset, reconciliation activity detection, latency
  diagnostics and external execution's durable intent requirement.
- Keep all daily Shenzhen symbols eligible for startup broker-position
  discovery, including zero static targets without external_delta. Preserve
  incoming external_delta validation and per-day quarantined worker exclusion.
- Retain both sets of tests, including the Shenzhen model sequence checks.
- The Shenzhen sampling runtime and V06 rules remain unchanged by the merge;
  prior market-cancel and empty-side self-best fixes are retained.

Validation:

- Full Release build with both stream processors, Deepwin adapters and both
  ATP plugins enabled, using the existing vendor SDK.
- 84 registered CTest cases: 82 passed initially; two launcher/fixture
  failures fixed and successfully rerun.
- SSE native gate fixture independently prepares its required lease
  directory. The Python launcher flushes stderr before exec so inherited
  configuration diagnostics are preserved.
- Additional Shenzhen zero-target/no-external-delta regression passes.
- Config suites: Shenzhen daily configuration and unified configuration.

Model golden-data tests that require an explicit external fixture directory
were not enabled. No production process, deployed library, live daily
configuration or timer is changed by this merge.
