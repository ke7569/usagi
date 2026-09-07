#include "sse_model_runtime.h"

#include <Eigen/Dense>

#include <cmath>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

namespace sse_model {
namespace {

typedef std::vector<float, Eigen::aligned_allocator<float> > Tensor;

const std::uint32_t kArtifactVersion = 1U;
const unsigned char kFactorHash[32] = {
    0x24, 0xfd, 0x61, 0xf8, 0xc4, 0x98, 0x27, 0x8d,
    0xd6, 0x7d, 0xb7, 0xf1, 0x83, 0xaa, 0x48, 0x46,
    0xae, 0x50, 0x07, 0x81, 0x43, 0xbb, 0xd3, 0x58,
    0x95, 0x82, 0x99, 0xf8, 0x81, 0x7d, 0xb0, 0x89
};

struct TensorSpec {
    std::size_t rows;
    std::size_t cols;
};

const TensorSpec kTensorSpecs[] = {
    {128, 50}, {128, 1},
    {512, 50}, {512, 1}, {256, 512}, {256, 1}, {128, 256}, {128, 1},
    {192, 128}, {192, 64}, {192, 1}, {192, 1},
    {64, 128}, {64, 1}, {64, 1}, {64, 1},
    {8, 64}, {8, 1}, {1, 8}, {1, 1}
};
const std::size_t kTensorCount = sizeof(kTensorSpecs) / sizeof(kTensorSpecs[0]);

bool finite_vector(const Tensor& values) {
    for (float value : values) if (!std::isfinite(value)) return false;
    return true;
}

float softsign(float value) {
    return value / (1.0f + std::fabs(value));
}

float sigmoid(float value) {
    if (value >= 0.0f) {
        const float e = std::exp(-value);
        return 1.0f / (1.0f + e);
    }
    const float e = std::exp(value);
    return e / (1.0f + e);
}

// All model dimensions are fixed by the artifact contract.  Using compile-time
// dimensions avoids Eigen's dynamic-size dispatch on every inference and lets
// the target compiler select the appropriate AVX/FMA micro-kernel.
template <int Rows, int Cols>
inline void matvec_fixed(const Tensor& weights,
                         const float* input, const float* bias,
                         float* output) {
    typedef Eigen::Matrix<float, Rows, Cols, Eigen::RowMajor> Matrix;
    typedef Eigen::Matrix<float, Cols, 1> Input;
    typedef Eigen::Matrix<float, Rows, 1> Output;
    const Eigen::Map<const Matrix> matrix(weights.data());
    const Eigen::Map<const Input> input_vector(input);
    Eigen::Map<Output> output_vector(output);
    output_vector.noalias() = matrix * input_vector;
    if (bias != 0) {
        output_vector += Eigen::Map<const Output>(bias);
    }
}

bool read_bytes(std::ifstream* input, void* data, std::size_t size) {
    input->read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return input->good() && static_cast<std::size_t>(input->gcount()) == size;
}

}  // namespace

struct Model::Impl {
    std::vector<Tensor> tensors;
};

State::State() : accepted_rows(0U) { reset(); }
void State::reset() { hidden.fill(0.0f); accepted_rows = 0U; }

Model::Model() : impl_(new Impl()) {}
Model::~Model() {}
Model::Model(Model&& other) noexcept : impl_(std::move(other.impl_)) {}
Model& Model::operator=(Model&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}

bool Model::load(const std::string& path, std::string* error) {
    if (error) error->clear();
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        if (error) *error = "cannot open SSE native model artifact: " + path;
        return false;
    }
    char magic[sizeof(kArtifactMagic) - 1U] = {};
    std::uint32_t version = 0U;
    unsigned char factor_hash[sizeof(kFactorHash)] = {};
    if (!read_bytes(&input, magic, sizeof(magic)) ||
        !read_bytes(&input, &version, sizeof(version)) ||
        !read_bytes(&input, factor_hash, sizeof(factor_hash)) ||
        std::memcmp(magic, kArtifactMagic, sizeof(magic)) != 0 ||
        version != kArtifactVersion ||
        std::memcmp(factor_hash, kFactorHash, sizeof(kFactorHash)) != 0) {
        if (error) *error = "SSE native model artifact header/version/factor contract mismatch";
        return false;
    }
    std::vector<Tensor> tensors;
    tensors.reserve(kTensorCount);
    for (std::size_t i = 0; i < kTensorCount; ++i) {
        const std::size_t count = kTensorSpecs[i].rows *
                                  (kTensorSpecs[i].cols == 1U ? 1U : kTensorSpecs[i].cols);
        Tensor values(count, 0.0f);
        if (!read_bytes(&input, values.data(), count * sizeof(float)) ||
            !finite_vector(values)) {
            if (error) *error = "SSE native model artifact tensor read/finite check failed";
            return false;
        }
        tensors.push_back(std::move(values));
    }
    impl_->tensors.swap(tensors);
    return true;
}

bool Model::loaded() const { return impl_ && impl_->tensors.size() == kTensorCount; }

bool Model::predict(const std::array<float, kFeatureCount>& factors,
                    State* state, float* prediction) const {
    if (!loaded() || state == 0 || prediction == 0) return false;
    for (float value : factors) if (!std::isfinite(value)) return false;

    const Tensor& proj_w = impl_->tensors[0];
    const Tensor& proj_b = impl_->tensors[1];
    const Tensor& f0_w = impl_->tensors[2];
    const Tensor& f0_b = impl_->tensors[3];
    const Tensor& f1_w = impl_->tensors[4];
    const Tensor& f1_b = impl_->tensors[5];
    const Tensor& f2_w = impl_->tensors[6];
    const Tensor& f2_b = impl_->tensors[7];
    const Tensor& gru_ih = impl_->tensors[8];
    const Tensor& gru_hh = impl_->tensors[9];
    const Tensor& gru_bih = impl_->tensors[10];
    const Tensor& gru_bhh = impl_->tensors[11];
    const Tensor& residual_w = impl_->tensors[12];
    const Tensor& residual_b = impl_->tensors[13];
    const Tensor& ln_w = impl_->tensors[14];
    const Tensor& ln_b = impl_->tensors[15];
    const Tensor& head0_w = impl_->tensors[16];
    const Tensor& head0_b = impl_->tensors[17];
    const Tensor& head2_w = impl_->tensors[18];
    const Tensor& head2_b = impl_->tensors[19];

    std::array<float, 128> projected;
    std::array<float, 512> layer0;
    std::array<float, 256> layer1;
    std::array<float, 128> nonlinear;
    std::array<float, 128> feature;
    matvec_fixed<512, 50>(f0_w, factors.data(), f0_b.data(), layer0.data());
    for (float& value : layer0) value = softsign(value);
    matvec_fixed<256, 512>(f1_w, layer0.data(), f1_b.data(), layer1.data());
    for (float& value : layer1) value = softsign(value);
    matvec_fixed<128, 256>(f2_w, layer1.data(), f2_b.data(), nonlinear.data());
    for (float& value : nonlinear) value = softsign(value);
    matvec_fixed<128, 50>(proj_w, factors.data(), proj_b.data(), projected.data());
    for (std::size_t i = 0; i < feature.size(); ++i) feature[i] = nonlinear[i] + projected[i];

    std::array<float, 192> ih;
    std::array<float, 192> hh;
    matvec_fixed<192, 128>(gru_ih, feature.data(), gru_bih.data(), ih.data());
    matvec_fixed<192, 64>(gru_hh, state->hidden.data(), gru_bhh.data(), hh.data());
    std::array<float, 64> recurrent;
    for (std::size_t i = 0; i < 64U; ++i) {
        const float reset_gate = sigmoid(ih[i] + hh[i]);
        const float update_gate = sigmoid(ih[64U + i] + hh[64U + i]);
        const float new_gate = std::tanh(ih[128U + i] + reset_gate * hh[128U + i]);
        recurrent[i] = new_gate + update_gate * (state->hidden[i] - new_gate);
    }
    std::array<float, 64> residual;
    matvec_fixed<64, 128>(residual_w, feature.data(), residual_b.data(), residual.data());

    std::array<float, 64> encoded;
    float mean = 0.0f;
    for (std::size_t i = 0; i < 64U; ++i) { encoded[i] = recurrent[i] + residual[i]; mean += encoded[i]; }
    mean /= 64.0f;
    float variance = 0.0f;
    for (std::size_t i = 0; i < 64U; ++i) { const float centered = encoded[i] - mean; variance += centered * centered; }
    variance /= 64.0f;
    const float inverse_std = 1.0f / std::sqrt(variance + 1.0e-5f);
    for (std::size_t i = 0; i < 64U; ++i) encoded[i] = (encoded[i] - mean) * inverse_std * ln_w[i] + ln_b[i];

    std::array<float, 8> head;
    matvec_fixed<8, 64>(head0_w, encoded.data(), head0_b.data(), head.data());
    for (float& value : head) value = softsign(value);
    float output = 0.0f;
    matvec_fixed<1, 8>(head2_w, head.data(), head2_b.data(), &output);
    if (!std::isfinite(output)) return false;
    *prediction = output;
    state->hidden = recurrent;
    ++state->accepted_rows;
    return true;
}

}  // namespace sse_model
