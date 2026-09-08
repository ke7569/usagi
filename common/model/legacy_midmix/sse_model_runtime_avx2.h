#ifndef SSE_T0_MODEL_RUNTIME_AVX2_H
#define SSE_T0_MODEL_RUNTIME_AVX2_H

#include "common/model/legacy_midmix/sse_model_runtime.h"

#include <cstddef>

namespace sse_model {
namespace avx2 {

// Pointers are borrowed from Model::Impl and remain valid for one predict call.
// Matrix entries use eight-row blocks; one-dimensional tensors stay row-major.
struct WeightsView {
    const float* tensors[20];
};

bool available();

bool predict(const float* factors, State* state, float* prediction,
             const WeightsView& weights);

}  // namespace avx2
}  // namespace sse_model

#endif  // SSE_T0_MODEL_RUNTIME_AVX2_H
