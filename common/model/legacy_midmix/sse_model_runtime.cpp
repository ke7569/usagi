#include "common/model/legacy_midmix/sse_model_runtime.h"
#include "common/model/legacy_midmix/sse_model_runtime_avx2.h"
#include "common/model/legacy_midmix/sse_model_runtime_eigen_avx2.h"

#include <Eigen/Dense>

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
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

void matvec(const Tensor& weights, std::size_t rows,
            std::size_t cols, const float* input, const float* bias,
            float* output) {
    typedef Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic,
                          Eigen::RowMajor> RowMatrix;
    const Eigen::Map<const RowMatrix> matrix(
        weights.data(), static_cast<Eigen::Index>(rows),
        static_cast<Eigen::Index>(cols));
    const Eigen::Map<const Eigen::VectorXf> input_vector(
        input, static_cast<Eigen::Index>(cols));
    Eigen::Map<Eigen::VectorXf> output_vector(
        output, static_cast<Eigen::Index>(rows));
    output_vector.noalias() = matrix * input_vector;
    if (bias != 0) {
        output_vector += Eigen::Map<const Eigen::VectorXf>(
            bias, static_cast<Eigen::Index>(rows));
    }
}

bool read_bytes(std::ifstream* input, void* data, std::size_t size) {
    input->read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return input->good() && static_cast<std::size_t>(input->gcount()) == size;
}

}  // namespace

struct Model::Impl {
    std::vector<Tensor> tensors;
    std::vector<std::vector<float> > avx2_tensors;
    std::vector<std::size_t> avx2_offsets;
    bool use_avx2;

    Impl() : use_avx2(false) {}
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
    std::vector<std::vector<float> > avx2_tensors;
    const bool use_avx2 = avx2::available();
    if (use_avx2) {
        avx2_tensors.resize(kTensorCount);
        std::vector<std::size_t> avx2_offsets(kTensorCount, 0U);
        for (std::size_t i = 0U; i < kTensorCount; ++i) {
            if (kTensorSpecs[i].cols == 1U) continue;
            const std::size_t packed_rows =
                ((kTensorSpecs[i].rows + 7U) / 8U) * 8U;
            // std::vector does not guarantee 32-byte alignment. Reserve eight
            // extra floats so the actual matrix can start at that boundary.
            avx2_tensors[i].assign(packed_rows * kTensorSpecs[i].cols + 8U,
                                   0.0f);
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(
                avx2_tensors[i].data());
            const std::size_t misalignment =
                static_cast<std::size_t>(address & 31U);
            avx2_offsets[i] = misalignment == 0U
                ? 0U : (32U - misalignment) / sizeof(float);
            float* packed = avx2_tensors[i].data() + avx2_offsets[i];
            for (std::size_t row = 0U; row < kTensorSpecs[i].rows; ++row) {
                for (std::size_t col = 0U; col < kTensorSpecs[i].cols; ++col) {
                    packed[(row / 8U) * kTensorSpecs[i].cols * 8U +
                           col * 8U + (row % 8U)] =
                        tensors[i][row * kTensorSpecs[i].cols + col];
                }
            }
        }
        impl_->avx2_offsets.swap(avx2_offsets);
    } else {
        impl_->avx2_offsets.clear();
    }
    impl_->tensors.swap(tensors);
    impl_->avx2_tensors.swap(avx2_tensors);
    impl_->use_avx2 = use_avx2;
    return true;
}

bool Model::loaded() const { return impl_ && impl_->tensors.size() == kTensorCount; }

bool Model::predict(const std::array<float, kFeatureCount>& factors,
                    State* state, float* prediction) const {
    if (!loaded() || state == 0 || prediction == 0) return false;
    for (float value : factors) if (!std::isfinite(value)) return false;

    if (impl_->use_avx2) {
        eigen_avx2::WeightsView eigen_weights = {};
        for (std::size_t i = 0U; i < kTensorCount; ++i)
            eigen_weights.tensors[i] = impl_->tensors[i].data();
        return eigen_avx2::predict(factors.data(), state, prediction,
                                   eigen_weights);
    }

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
    matvec(f0_w, 512, 50, factors.data(), f0_b.data(), layer0.data());
    for (float& value : layer0) value = softsign(value);
    matvec(f1_w, 256, 512, layer0.data(), f1_b.data(), layer1.data());
    for (float& value : layer1) value = softsign(value);
    matvec(f2_w, 128, 256, layer1.data(), f2_b.data(), nonlinear.data());
    for (float& value : nonlinear) value = softsign(value);
    matvec(proj_w, 128, 50, factors.data(), proj_b.data(), projected.data());
    for (std::size_t i = 0; i < feature.size(); ++i) feature[i] = nonlinear[i] + projected[i];

    std::array<float, 192> ih;
    std::array<float, 192> hh;
    matvec(gru_ih, 192, 128, feature.data(), gru_bih.data(), ih.data());
    matvec(gru_hh, 192, 64, state->hidden.data(), gru_bhh.data(), hh.data());
    std::array<float, 64> recurrent;
    for (std::size_t i = 0; i < 64U; ++i) {
        const float reset_gate = sigmoid(ih[i] + hh[i]);
        const float update_gate = sigmoid(ih[64U + i] + hh[64U + i]);
        const float new_gate = std::tanh(ih[128U + i] + reset_gate * hh[128U + i]);
        recurrent[i] = new_gate + update_gate * (state->hidden[i] - new_gate);
    }
    std::array<float, 64> residual;
    matvec(residual_w, 64, 128, feature.data(), residual_b.data(), residual.data());

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
    matvec(head0_w, 8, 64, encoded.data(), head0_b.data(), head.data());
    for (float& value : head) value = softsign(value);
    float output = 0.0f;
    matvec(head2_w, 1, 8, head.data(), head2_b.data(), &output);
    if (!std::isfinite(output)) return false;
    *prediction = output;
    state->hidden = recurrent;
    ++state->accepted_rows;
    return true;
}

}  // namespace sse_model
