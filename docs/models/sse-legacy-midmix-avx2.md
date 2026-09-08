# SSE Legacy MidMix AVX2 Runtime

This change keeps the public `sse_model::Model` ABI and the original Eigen
implementation. On x86, model load checks CPUID, OSXSAVE, AVX, XCR0 YMM state,
and CPUID leaf 7 AVX2. If all checks pass, the loader creates an internal
eight-row packed copy of the nine matrix tensors. `predict` dispatches once per
loaded model to fixed-size AVX2 matrix-vector kernels; unsupported CPUs use the
existing Eigen/SSE path without the packed allocation.

The AVX2 kernel uses explicit multiply followed by add and is compiled with
`-ffp-contract=off`; it does not use FMA, fast-math, a global `-march=native`,
or a reduced model topology. Softsign, sigmoid, tanh, LayerNorm, recurrent
state commit, finite checks, and the tensor order are unchanged. A model state
is still sequential: rows sharing one `State` cannot be reordered or run in
parallel. Different instruments remain independently parallelizable at the
caller.

## Verification

Build environment: GCC 4.8.5, `-O3 -std=gnu++11`, global target
`-march=x86-64 -mtune=generic`; only
`sse_model_runtime_avx2.cpp` receives `-mavx2`. The existing
`sse_model_runtime_test` passes.

The real tick artifact `/home/zane/sse/artifacts/tick/ssemodl1.bin` was loaded
by both the baseline library and the optimized library. On CPU 64 of the AMD
EPYC 9755 host, pinned with `taskset -c 64`, the in-memory benchmark used the
same deterministic 4,102-row, 50-factor Float32 sequence for ten repetitions:

| Runtime | TSC cycles per row |
| --- | ---: |
| baseline Eigen/SSE | 312,329 |
| packed AVX2 | 237,580 |

This is an isolated `Model::predict` benchmark; factor construction, order
book updates, snapshot factors, hybrid routing, and the caller's exchange
string are outside its timed region. TSC cycles are reported rather than
claiming a fixed wall-clock nanosecond value.

The baseline and optimized sequence probe outputs contain 4,166 floats (4,102
predictions plus the final 64-element hidden state). The maximum absolute
difference is `4.29153e-6`; the largest relative difference is `2.28354e-2`
at a near-zero value. There are no non-finite outputs or recurrent-state
divergences. The small absolute drift is from the packed AVX2 dot-product
accumulation order; no model weights or samples are dropped.

The snapshot GRU runtime and `snapshot_ensemble` are intentionally unchanged:
they have a separate 36/95-factor ABI and serial two-arm state commit contract.
