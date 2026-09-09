#include "common/model/legacy_midmix/sse_model_runtime_avx2.h"

#include <cpuid.h>
#include <immintrin.h>

#include <cmath>

namespace sse_model {
namespace avx2 {
namespace {

const std::size_t kTensorCount = 20U;

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

// The packed layout is [row_block][input_column][row_in_block].  Keeping the
// input loop in source order makes each output lane's scalar term order stable
// and avoids fused multiply-add contraction.
inline void matvec(const float* packed, std::size_t rows, std::size_t cols,
                   const float* input, const float* bias, float* output) {
    std::size_t row = 0U;
    for (; row + 8U <= rows; row += 8U) {
        const float* block = packed + (row / 8U) * cols * 8U;
        __m256 sum0 = _mm256_setzero_ps();
        __m256 sum1 = _mm256_setzero_ps();
        __m256 sum2 = _mm256_setzero_ps();
        __m256 sum3 = _mm256_setzero_ps();
        std::size_t col = 0U;
        for (; col + 3U < cols; col += 4U) {
            sum0 = _mm256_fmadd_ps(
                _mm256_load_ps(block + col * 8U), _mm256_set1_ps(input[col]), sum0);
            sum1 = _mm256_fmadd_ps(
                _mm256_load_ps(block + (col + 1U) * 8U),
                _mm256_set1_ps(input[col + 1U]), sum1);
            sum2 = _mm256_fmadd_ps(
                _mm256_load_ps(block + (col + 2U) * 8U),
                _mm256_set1_ps(input[col + 2U]), sum2);
            sum3 = _mm256_fmadd_ps(
                _mm256_load_ps(block + (col + 3U) * 8U),
                _mm256_set1_ps(input[col + 3U]), sum3);
        }
        __m256 sum = _mm256_add_ps(_mm256_add_ps(sum0, sum1),
                                   _mm256_add_ps(sum2, sum3));
        for (; col < cols; ++col)
            sum = _mm256_fmadd_ps(
                _mm256_load_ps(block + col * 8U), _mm256_set1_ps(input[col]), sum);
        if (bias != 0) {
            sum = _mm256_add_ps(sum, _mm256_loadu_ps(bias + row));
        }
        _mm256_storeu_ps(output + row, sum);
    }

    if (row < rows) {
        const float* block = packed + (row / 8U) * cols * 8U;
        for (std::size_t lane = 0U; row + lane < rows; ++lane) {
            float sum = 0.0f;
            for (std::size_t col = 0U; col < cols; ++col) {
                sum += block[col * 8U + lane] * input[col];
            }
            output[row + lane] = sum + (bias == 0 ? 0.0f : bias[row + lane]);
        }
    }
}

}  // namespace

bool available() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int max_leaf = __get_cpuid_max(0U, 0);
    if (max_leaf < 7U) return false;

    unsigned int eax = 0U;
    unsigned int ebx = 0U;
    unsigned int ecx = 0U;
    unsigned int edx = 0U;
    __cpuid(1U, eax, ebx, ecx, edx);
    const unsigned int osxsave = 1U << 27;
    const unsigned int avx = 1U << 28;
    if ((ecx & osxsave) == 0U || (ecx & avx) == 0U) return false;

    unsigned int xcr0_low = 0U;
    unsigned int xcr0_high = 0U;
    __asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
    (void)xcr0_high;
    if ((xcr0_low & 0x6U) != 0x6U) return false;

    __cpuid_count(7U, 0U, eax, ebx, ecx, edx);
    return (ebx & (1U << 5)) != 0U;
#else
    return false;
#endif
}

bool predict(const float* factors, State* state, float* prediction,
             const WeightsView& weights) {
    const float* proj_w = weights.tensors[0];
    const float* proj_b = weights.tensors[1];
    const float* f0_w = weights.tensors[2];
    const float* f0_b = weights.tensors[3];
    const float* f1_w = weights.tensors[4];
    const float* f1_b = weights.tensors[5];
    const float* f2_w = weights.tensors[6];
    const float* f2_b = weights.tensors[7];
    const float* gru_ih = weights.tensors[8];
    const float* gru_hh = weights.tensors[9];
    const float* gru_bih = weights.tensors[10];
    const float* gru_bhh = weights.tensors[11];
    const float* residual_w = weights.tensors[12];
    const float* residual_b = weights.tensors[13];
    const float* ln_w = weights.tensors[14];
    const float* ln_b = weights.tensors[15];
    const float* head0_w = weights.tensors[16];
    const float* head0_b = weights.tensors[17];
    const float* head2_w = weights.tensors[18];
    const float* head2_b = weights.tensors[19];

    std::array<float, 128> projected;
    std::array<float, 512> layer0;
    std::array<float, 256> layer1;
    std::array<float, 128> nonlinear;
    std::array<float, 128> feature;
    matvec(f0_w, 512U, 50U, factors, f0_b, layer0.data());
    for (float& value : layer0) value = softsign(value);
    matvec(f1_w, 256U, 512U, layer0.data(), f1_b, layer1.data());
    for (float& value : layer1) value = softsign(value);
    matvec(f2_w, 128U, 256U, layer1.data(), f2_b, nonlinear.data());
    for (float& value : nonlinear) value = softsign(value);
    matvec(proj_w, 128U, 50U, factors, proj_b, projected.data());
    for (std::size_t i = 0U; i < feature.size(); ++i) {
        feature[i] = nonlinear[i] + projected[i];
    }

    std::array<float, 192> ih;
    std::array<float, 192> hh;
    matvec(gru_ih, 192U, 128U, feature.data(), gru_bih, ih.data());
    matvec(gru_hh, 192U, 64U, state->hidden.data(), gru_bhh, hh.data());
    std::array<float, 64> recurrent;
    for (std::size_t i = 0U; i < 64U; ++i) {
        const float reset_gate = sigmoid(ih[i] + hh[i]);
        const float update_gate = sigmoid(ih[64U + i] + hh[64U + i]);
        const float new_gate = std::tanh(ih[128U + i] +
                                         reset_gate * hh[128U + i]);
        recurrent[i] = new_gate + update_gate * (state->hidden[i] - new_gate);
    }
    std::array<float, 64> residual;
    matvec(residual_w, 64U, 128U, feature.data(), residual_b, residual.data());

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
    for (std::size_t i = 0U; i < 64U; ++i) {
        encoded[i] = (encoded[i] - mean) * inverse_std * ln_w[i] + ln_b[i];
    }

    std::array<float, 8> head;
    matvec(head0_w, 8U, 64U, encoded.data(), head0_b, head.data());
    for (float& value : head) value = softsign(value);
    float output = 0.0f;
    matvec(head2_w, 1U, 8U, head.data(), head2_b, &output);
    if (!std::isfinite(output)) return false;
    *prediction = output;
    state->hidden = recurrent;
    ++state->accepted_rows;
    return true;
}

}  // namespace avx2
}  // namespace sse_model
