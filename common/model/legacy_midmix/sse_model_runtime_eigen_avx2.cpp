#include "common/model/legacy_midmix/sse_model_runtime_eigen_avx2.h"

#include <Eigen/Dense>

#include <cmath>

namespace sse_model {
namespace eigen_avx2 {
namespace {

inline float softsign(float value) {
    return value / (1.0f + std::fabs(value));
}

inline float sigmoid(float value) {
    if (value >= 0.0f) {
        const float e = std::exp(-value);
        return 1.0f / (1.0f + e);
    }
    const float e = std::exp(value);
    return e / (1.0f + e);
}

template <int Rows, int Cols>
inline void matvec(const float* weights, const float* input,
                   const float* bias, float* output) {
    typedef Eigen::Matrix<float, Rows, Cols, Eigen::RowMajor> Matrix;
    typedef Eigen::Matrix<float, Cols, 1> Input;
    typedef Eigen::Matrix<float, Rows, 1> Output;
    const Eigen::Map<const Matrix, Eigen::Aligned> matrix(weights);
    const Eigen::Map<const Input, Eigen::Unaligned> input_vector(input);
    Eigen::Map<Output, Eigen::Unaligned> output_vector(output);
    output_vector.noalias() = matrix * input_vector;
    if (bias != 0)
        output_vector += Eigen::Map<const Output, Eigen::Unaligned>(bias);
}

}  // namespace

bool predict(const float* factors, State* state, float* prediction,
             const WeightsView& weights) {
    std::array<float, 128> projected;
    std::array<float, 512> layer0;
    std::array<float, 256> layer1;
    std::array<float, 128> nonlinear;
    std::array<float, 128> feature;
    matvec<512, 50>(weights.tensors[2], factors, weights.bias(3), layer0.data());
    for (float& value : layer0) value = softsign(value);
    matvec<256, 512>(weights.tensors[4], layer0.data(), weights.bias(5), layer1.data());
    for (float& value : layer1) value = softsign(value);
    matvec<128, 256>(weights.tensors[6], layer1.data(), weights.bias(7), nonlinear.data());
    for (float& value : nonlinear) value = softsign(value);
    matvec<128, 50>(weights.tensors[0], factors, weights.bias(1), projected.data());
    for (std::size_t i = 0U; i < feature.size(); ++i)
        feature[i] = nonlinear[i] + projected[i];

    std::array<float, 192> ih;
    std::array<float, 192> hh;
    matvec<192, 128>(weights.tensors[8], feature.data(), weights.bias(10), ih.data());
    matvec<192, 64>(weights.tensors[9], state->hidden.data(), weights.bias(11), hh.data());
    std::array<float, 64> recurrent;
    for (std::size_t i = 0U; i < 64U; ++i) {
        const float reset_gate = sigmoid(ih[i] + hh[i]);
        const float update_gate = sigmoid(ih[64U + i] + hh[64U + i]);
        const float new_gate = std::tanh(ih[128U + i] + reset_gate * hh[128U + i]);
        recurrent[i] = new_gate + update_gate * (state->hidden[i] - new_gate);
    }
    std::array<float, 64> residual;
    matvec<64, 128>(weights.tensors[12], feature.data(), weights.bias(13), residual.data());

    std::array<float, 64> encoded;
    float mean = 0.0f;
    for (std::size_t i = 0U; i < 64U; ++i) {
        encoded[i] = recurrent[i] + residual[i];
        mean += encoded[i];
    }
    mean /= 64.0f;
    float variance = 0.0f;
    for (std::size_t i = 0U; i < 64U; ++i) {
        const float centered = encoded[i] - mean;
        variance += centered * centered;
    }
    variance /= 64.0f;
    const float inverse_std = 1.0f / std::sqrt(variance + 1.0e-5f);
    for (std::size_t i = 0U; i < 64U; ++i)
        encoded[i] = (encoded[i] - mean) * inverse_std *
                     weights.bias(14)[i] + weights.bias(15)[i];

    std::array<float, 8> head;
    matvec<8, 64>(weights.tensors[16], encoded.data(), weights.bias(17), head.data());
    for (float& value : head) value = softsign(value);
    float output = 0.0f;
    matvec<1, 8>(weights.tensors[18], head.data(), weights.bias(19), &output);
    if (!std::isfinite(output)) return false;
    *prediction = output;
    state->hidden = recurrent;
    ++state->accepted_rows;
    return true;
}

}  // namespace eigen_avx2
}  // namespace sse_model
