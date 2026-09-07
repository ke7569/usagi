## Why

The repository still presents Shanghai as a temporary Shenzhen child project. Shared code, market differences, model implementations and deployment tools are mixed under historical paths, obscuring ownership and making live/replay maintenance harder.

## What Changes

- Rename the source project to `usagi`, with symmetric `sze/` and `sse/` differences around shared implementations in `common/`.
- Move model implementations by actual architecture, not exchange; retain separate weights, feature contracts and mutable instrument state.
- Separate application assembly, SDK adapters, configuration examples, tools and tests. Consolidate current documentation behind one README.
- Preserve the dirty worktree, source provenance, trading behavior, model arithmetic, recording formats, exported ABI and production installation paths.
- Update repository-local references and verify independent market builds, offline behavior and packaging. GitHub publication and production activation are separate from moving source files.

## Capabilities

### New Capabilities

- `usagi-source-layout`: Discoverable ownership, relocation-safe tooling and documented compatibility for the unified repository.

### Modified Capabilities

None. Existing market-stream, sampling and execution requirements remain unchanged.

## Impact

Root CMake, source/include locations, Python and shell tools, tests, examples and documentation change. Existing plugin/CLI names, source IDs, market IDs and live execution restrictions stay intact. No broker connection, deployment, credentials migration, model retraining or automatic removal of unfinished work is included.
