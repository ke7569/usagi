## Context

The starting checkout is `feature/sse-t0` at `029660f8b608e39ae35cea5f63dc15523201c3a8`, including substantial uncommitted work. Its prior stream-runtime change remains incomplete at legacy SSE host integration. This migration must preserve that limitation and the paper-only execution boundary.

## Goals / Non-Goals

Goals: one `usagi` source root, common implementations, symmetric market difference directories, relocation-safe build/tooling and a small current documentation entry point.

Non-goals: changing arithmetic, sampling semantics, model topology/weights, journal or plugin ABI, production paths, broker support or account readiness. No new AGENTS.md. No automatic archive of incomplete changes or production deployment.

## Decisions

- Use `common/` for existing shared stream/strategy/execution code, portable compatibility types, and model engines grouped by architecture. Keep model namespaces and artifact contracts unchanged. Do not pretend different topologies are interchangeable weights.
- Use symmetric `sze/` and `sse/` ownership: `market_data`, `sampling`, `factors`, `model` and thin `runtime` bindings where needed. Existing coupled sampling/book code moves without being algorithmically split in this change.
- Keep market-selecting assembly in `apps/`, not in lower-level common libraries. Keep Deepwin plugins in `adapters/deepwin` and the existing ATP implementation in `adapters/td/atp`. No speculative Guojun adapter.
- Centralize configuration examples, tests, tools and deployment scripts. Preserve unified schema v1 and its existing projections; legacy examples are explicitly labeled, not a second new schema.
- Record an explicit per-file move map. Rewrite only resolvable local includes and mapped repository paths. Do not replace market/source IDs, artifact hashes, external `/opt` or deployed account directories. Vendor content and historical evidence are not bulk rewritten.
- Keep root build configuration and small component source manifests, with SDK targets selectable independently of portable stream targets. Preserve external target/output names during migration.
- README links current architecture, configuration and operations guides; detailed contracts remain linked. Historical design and execution records retain their original provenance and are labeled historical instead of silently rewritten as new facts.

## Risks / Trade-offs

- Dirty files or ignored assets lost: retain an external source snapshot and pre-move hashes; fail on destination collisions and preserve unmapped assets.
- Relative paths break: resolve includes before moves, test tools from outside the checkout and validate package manifests.
- Numerical drift hidden by tests: preserve non-path source bodies and compiler flags; compare fixed-input outputs using the same baseline model fixtures.
- Legacy code still coupled: label compatibility code honestly; further factor/runtime extraction is a separately verified behavior-preserving change, not an excuse to claim this migration unifies every algorithm.
- Offline success confused with live acceptance: report real TD, old SSE host lifecycle and production-model/full-day verification separately.

## Migration Plan

1. Preserve baseline, run existing tests and record exact file mapping.
2. Relocate source and tests; update includes and build source lists while preserving ABI and arithmetic.
3. Relocate tools/config/deployment references and consolidate current documentation.
4. Rename the source directory to `/home/usagi`; rebuild in a new ignored build directory and verify independent market targets, old output names and offline regressions.
5. Retain the external baseline for recovery. Source migration does not overwrite any deployed package. GitHub repository naming/publication is handled separately after review of the resulting diff and remote destination.

## Verification

Local source relocation completed at `/home/usagi` on 2026-09-06. No AGENTS.md, commit, remote change, push or production deployment was made.

- Baseline: 29/29 CTest before relocation. Snapshot `/home/ref/usagi-migration-baseline-20260906-kVbrdl/worktree.tar.gz`, SHA-256 `4e65445a03acff79d0c8ccf12227ec5557ee1b4d0642e11698fe131db0bbfb1a`.
- Integrity: all 874 mapped targets exist; all 794 Git deletions correspond to recorded moves; 198 C++/header non-include body hashes and 531 Eigen/JSON vendor hashes match. Existing generated directories are retained under `build/legacy-*` and `build/migration-preserved`; no original data was deleted.
- Fresh portable build: `build/verification`, GCC 4.8.5, both markets explicitly ON, 34/34 CTest. The suite includes the original 29 targets plus layout, deployment-path and configuration suites; configuration groups contain 28 unified, 27 processing and 12 daily cases.
- Independent builds: `build/verify-sze-only` (SZE ON/SSE OFF), 20/20 CTest; `build/verify-sse-isolated` (SZE OFF/SSE ON), 21/21 CTest. Runtime link inputs contain only the selected market/model. An earlier incorrectly configured `verify-sse-only` directory is not isolation evidence.
- Exact pre/post replay comparison uses the same recorded receive timing and synthetic models with the binaries in `/home/ref/md-stream-build-20260906-rYn4jN` and `build/verification`. SZE: 197 samples/predictions, factor CRC32 1413721026, 196 paper orders. SSE: 6 samples/predictions, factor CRC32 3574447231, one paper order and one cancel. The complete processing summaries match; results and reproduction script are retained in the external baseline directory.
- SDK compatibility: the validated CentOS 7.9/GCC 4.8.5 environment built both legacy strategy plugins and SZE MD/L1 plugins in `build/verify-deepwin`, and both ATP TD plugins independently of the strategy build in `build/verify-td`. Existing `get_obj`, runtime C API, output names and namespaced framework ABI remain; local `ldd -r` checks pass with the explicitly supplied SDK runtime search path for TD.
- Relocation tooling: unified SZE migration succeeds from `/tmp`; capture-package checksums pass; source/package launchers are tested with a stub that never connects to market data. SSE daily requires its external metadata validator, and legacy runtime packaging requires explicit artifacts. Production-model bundle conversion still requires the original training handoffs and is not a live acceptance claim.
- Additional Python checks: the SZE tool suite runs 29 cases with one existing model-artifact case skipped because its checked-in model is unavailable; both replay comparator scripts and the SSE model skeleton check pass. These are separate from the 34/34 CTest result.

Remaining work is unchanged: real TD/reconciliation and in-flight orders, legacy SSE host lifecycle integration, production-model/full-day replay, live burst/tail-latency acceptance, further verified factor/sampling extraction, and separately reviewed GitHub publication. The source snapshot and existing deployed packages are the recovery path; no installed runtime was switched.
