#ifndef SSE_V06_MATRIX_KERNELS_H
#define SSE_V06_MATRIX_KERNELS_H
namespace sse_v06 { namespace matrix {
// Checks hardware CPUID and OS XCR0 directly. This host's old libgcc reports
// false through newer compiler CPU builtins even though ZMM state is enabled.
bool avx512_available();
// Weights are packed in blocks of sixteen output rows, column-major within
// each block. Shapes are fixed by the v0.6 artifact contract.
void linear_gru_avx512(const float*,const float*,const float*,float*);
void linear_projection_avx512(const float*,const float*,const float*,float*);
}}
#endif
