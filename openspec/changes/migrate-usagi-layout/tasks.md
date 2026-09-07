## 1. Baseline

- [x] 1.1 Preserve the existing worktree and record baseline test results and a collision-checked file mapping (external snapshot, 29/29 CTest, file-map.json; C++ bodies unchanged by relocation).

## 2. Source Layout

- [x] 2.1 Relocate common implementations, symmetric market differences, model architectures, SDK adapters and application assembly; preserve non-path source behavior (874 mapped files present; 198 C++/header body hashes unchanged; 531 Eigen/JSON vendor hashes unchanged).
- [x] 2.2 Update explicit build sources and dependency boundaries; retain legacy output names and independently selectable portable markets (original target/test source lists preserved; both strategy/MD plugins and independent ATP TD targets built).

## 3. Tooling and Documentation

- [x] 3.1 Centralize configuration examples, tools, tests and deployment sources; verify repository discovery after relocation without changing external runtime paths (67 existing config cases; out-of-tree migration CLI; package checksums and three stub-only deployment-path tests).
- [x] 3.2 Consolidate current documentation and retain historical evidence with a clear status and source-path mapping (README plus three current guides; detailed contracts and historical evidence separated; local documentation links verified).

## 4. Acceptance

- [x] 4.1 Rename the local source root/project to Usagi and verify a fresh build, both market regressions, configuration tests and fixed-input pre/post behavior (/home/usagi; fresh build/verification; 34/34 CTest; exact pre/post processing parity with the same SZE/SSE synthetic-model recordings).
- [x] 4.2 Verify independent market builds, dependency/export boundaries, packaging references and migration integrity; report remaining live/TD work and publication status (SZE-only 20/20, SSE-only 21/21; no cross-market runtime/model libraries; local SDK link checks; no commit, push or production deployment).
