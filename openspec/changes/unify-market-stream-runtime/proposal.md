## Why

Shanghai burst feeds and Shenzhen continuous feeds need one auditable ingress and replay implementation without merging their market semantics. The approved scope proceeds through both market integrations, separating recording degradation from invalid market data and keeping production deployment out of scope.

## What Changes

- Finish bounded, preallocated batch receive and asynchronous full-payload recording with exact event replay.
- Separate required recording from optional diagnostic recording; never treat a recorder-only error as proof that the order book is invalid.
- Establish independently buildable market processing targets and reuse real decoders, books, samplers, factors and models for live input and replay.
- Preserve market-specific sequence recovery and sampling; compare observable outputs with deterministic fixtures.
- Document remaining actual-feed, full-day model and SDK verification separately from local test results.

## Capabilities

### New Capabilities

- `unified-market-stream`: Shared transport, recording policy, deterministic market processing and independent market builds.

### Modified Capabilities

None. Existing deployed entry points remain compatibility paths until verified integration replaces them.

## Impact

Changes are scoped to `modules/deepwin_guoxin/md/common`, market processing adapters, related CMake targets, tests and documentation. Selected source dependencies may be imported from the fixed reviewed SSE/SZE snapshots with provenance. Existing user edits, SDK ABI, strategy thresholds and production files are preserved. No TD login, order submission, push, final branch merge or deployment is authorized.
