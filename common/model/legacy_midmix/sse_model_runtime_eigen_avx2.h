#ifndef SSE_T0_MODEL_RUNTIME_EIGEN_AVX2_H
#define SSE_T0_MODEL_RUNTIME_EIGEN_AVX2_H

#include "common/model/legacy_midmix/sse_model_runtime.h"

#include <cstddef>

namespace sse_model {
namespace eigen_avx2 {

struct WeightsView {
    const float* tensors[20];

    const float* bias(std::size_t index) const { return tensors[index]; }
};

bool predict(const float* factors, State* state, float* prediction,
             const WeightsView& weights);

}  // namespace eigen_avx2
}  // namespace sse_model

#endif  // SSE_T0_MODEL_RUNTIME_EIGEN_AVX2_H
