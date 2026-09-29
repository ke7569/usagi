#include "snapshot_gru_runtime.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

#include "third_party/nlohmann/json.hpp"

namespace sse_snapshot_gru {
namespace {

const char kArtifactMagic[8] = {'S','S','E','S','G','R','U','1'};
const std::uint32_t kArtifactVersion = 1U;
const std::size_t kTensorCount = 20U;

struct Tensor {
    std::vector<std::uint32_t> shape;
    std::vector<float> values;
};

bool read_bytes(std::ifstream* input, void* data, std::size_t size) {
    if (!input->read(reinterpret_cast<char*>(data),
                     static_cast<std::streamsize>(size))) return false;
    return static_cast<std::size_t>(input->gcount()) == size;
}

template <typename T>
bool read_exact(std::ifstream* input, T* value) {
    return read_bytes(input, value, sizeof(T));
}

bool finite_vector(const std::vector<float>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}

bool same_shape(const std::vector<std::uint32_t>& actual,
                const std::vector<std::uint32_t>& expected) {
    return actual == expected;
}

bool assign_tensor(const std::vector<std::pair<std::string, Tensor> >& tensors,
                   const char* name, const std::vector<std::uint32_t>& shape,
                   Tensor* output, std::string* error) {
    for (std::size_t i = 0; i < tensors.size(); ++i) {
        if (tensors[i].first == name) {
            if (!same_shape(tensors[i].second.shape, shape)) {
                if (error) *error = std::string("tensor shape mismatch: ") + name;
                return false;
            }
            *output = tensors[i].second;
            return true;
        }
    }
    if (error) *error = std::string("missing tensor: ") + name;
    return false;
}

std::vector<std::uint32_t> shape2(std::uint32_t rows, std::uint32_t cols) {
    std::vector<std::uint32_t> result(2U);
    result[0] = rows; result[1] = cols;
    return result;
}

std::vector<std::uint32_t> shape1(std::uint32_t size) {
    std::vector<std::uint32_t> result(1U);
    result[0] = size;
    return result;
}

void matvec(const std::vector<float>& weights, std::size_t rows,
            std::size_t cols, const float* input, const float* bias,
            float* output) {
    for (std::size_t row = 0; row < rows; ++row) {
        float value = bias == 0 ? 0.0f : bias[row];
        const std::size_t offset = row * cols;
        for (std::size_t col = 0; col < cols; ++col) {
            value += weights[offset + col] * input[col];
        }
        output[row] = value;
    }
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

bool read_tensor(std::ifstream* input, std::string* name, Tensor* tensor,
                 std::string* error) {
    std::uint16_t name_len = 0U;
    std::uint8_t rank = 0U;
    std::uint8_t reserved[3] = {};
    std::uint64_t byte_count = 0U;
    if (!read_exact(input, &name_len) || name_len == 0U || name_len > 255U) {
        if (error) *error = "invalid tensor name length";
        return false;
    }
    name->assign(name_len, '\0');
    if (!read_bytes(input, &(*name)[0], name_len) ||
        !read_exact(input, &rank) || rank == 0U || rank > 4U ||
        !read_bytes(input, reserved, sizeof(reserved))) {
        if (error) *error = "truncated tensor header";
        return false;
    }
    tensor->shape.assign(rank, 0U);
    std::uint64_t elements = 1U;
    for (std::size_t i = 0; i < rank; ++i) {
        if (!read_exact(input, &tensor->shape[i]) || tensor->shape[i] == 0U ||
            elements > 100000000ULL / tensor->shape[i]) {
            if (error) *error = "invalid tensor shape: " + *name;
            return false;
        }
        elements *= tensor->shape[i];
    }
    if (!read_exact(input, &byte_count) ||
        byte_count != elements * sizeof(float) || elements > 100000000ULL) {
        if (error) *error = "invalid tensor byte count: " + *name;
        return false;
    }
    tensor->values.assign(static_cast<std::size_t>(elements), 0.0f);
    if (!read_bytes(input, tensor->values.data(),
                    tensor->values.size() * sizeof(float)) ||
        !finite_vector(tensor->values)) {
        if (error) *error = "truncated or non-finite tensor: " + *name;
        return false;
    }
    return true;
}

}  // namespace

struct Model::Impl {
    std::size_t feature_count;
    Tensor proj_weight, proj_bias;
    Tensor f0_weight, f0_bias, f2_weight, f2_bias, f4_weight, f4_bias;
    Tensor gru_ih, gru_hh, gru_bih, gru_bhh;
    Tensor res_weight, res_bias, ln_weight, ln_bias;
    Tensor head0_weight, head0_bias, head2_weight, head2_bias;
    std::vector<float> mean;
    std::vector<float> scale;
    bool loaded;

    Impl() : feature_count(0U), loaded(false) {}
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

bool Model::load(const std::string& weights_path,
                 const std::string& scaler_path,
                 std::size_t expected_feature_count,
                 std::string* error) {
    if (error) error->clear();
    impl_.reset(new Impl());
    std::ifstream input(weights_path.c_str(), std::ios::binary);
    if (!input) {
        if (error) *error = "cannot open SSE snapshot GRU artifact: " + weights_path;
        return false;
    }
    char magic[sizeof(kArtifactMagic)] = {};
    std::uint32_t version = 0U, feature_count = 0U, hidden_size = 0U;
    std::uint32_t tensor_count = 0U, reserved = 0U;
    if (!read_bytes(&input, magic, sizeof(magic)) ||
        !read_exact(&input, &version) || !read_exact(&input, &feature_count) ||
        !read_exact(&input, &hidden_size) || !read_exact(&input, &tensor_count) ||
        !read_exact(&input, &reserved) ||
        std::memcmp(magic, kArtifactMagic, sizeof(magic)) != 0 ||
        version != kArtifactVersion || hidden_size != kHiddenSize ||
        tensor_count != kTensorCount ||
        (feature_count != 36U && feature_count != 95U) ||
        (expected_feature_count != 0U && feature_count != expected_feature_count)) {
        if (error) *error = "SSE snapshot GRU artifact header mismatch";
        return false;
    }
    std::vector<std::pair<std::string, Tensor> > tensors;
    tensors.reserve(kTensorCount);
    for (std::size_t i = 0; i < kTensorCount; ++i) {
        std::string name; Tensor tensor;
        if (!read_tensor(&input, &name, &tensor, error)) return false;
        for (std::size_t j = 0; j < tensors.size(); ++j) {
            if (tensors[j].first == name) {
                if (error) *error = "duplicate tensor: " + name;
                return false;
            }
        }
        tensors.push_back(std::make_pair(name, tensor));
    }

    const std::vector<std::uint32_t> d128 = shape1(128U);
    const std::vector<std::uint32_t> d256 = shape1(256U);
    const std::vector<std::uint32_t> d512 = shape1(512U);
    const std::vector<std::uint32_t> d192 = shape1(192U);
    const std::vector<std::uint32_t> d64 = shape1(64U);
    const std::vector<std::uint32_t> d8 = shape1(8U);
    if (!assign_tensor(tensors, "proj.weight", shape2(128U, feature_count),
                       &impl_->proj_weight, error) ||
        !assign_tensor(tensors, "proj.bias", d128, &impl_->proj_bias, error) ||
        !assign_tensor(tensors, "feature_layers.0.weight", shape2(512U, feature_count),
                       &impl_->f0_weight, error) ||
        !assign_tensor(tensors, "feature_layers.0.bias", d512, &impl_->f0_bias, error) ||
        !assign_tensor(tensors, "feature_layers.2.weight", shape2(256U, 512U),
                       &impl_->f2_weight, error) ||
        !assign_tensor(tensors, "feature_layers.2.bias", d256, &impl_->f2_bias, error) ||
        !assign_tensor(tensors, "feature_layers.4.weight", shape2(128U, 256U),
                       &impl_->f4_weight, error) ||
        !assign_tensor(tensors, "feature_layers.4.bias", d128, &impl_->f4_bias, error) ||
        !assign_tensor(tensors, "gru.weight_ih_l0", shape2(192U, 128U),
                       &impl_->gru_ih, error) ||
        !assign_tensor(tensors, "gru.weight_hh_l0", shape2(192U, 64U),
                       &impl_->gru_hh, error) ||
        !assign_tensor(tensors, "gru.bias_ih_l0", d192, &impl_->gru_bih, error) ||
        !assign_tensor(tensors, "gru.bias_hh_l0", d192, &impl_->gru_bhh, error) ||
        !assign_tensor(tensors, "res_gru.weight", shape2(64U, 128U),
                       &impl_->res_weight, error) ||
        !assign_tensor(tensors, "res_gru.bias", d64, &impl_->res_bias, error) ||
        !assign_tensor(tensors, "ln.weight", d64, &impl_->ln_weight, error) ||
        !assign_tensor(tensors, "ln.bias", d64, &impl_->ln_bias, error) ||
        !assign_tensor(tensors, "prediction_head.0.weight", shape2(8U, 64U),
                       &impl_->head0_weight, error) ||
        !assign_tensor(tensors, "prediction_head.0.bias", d8, &impl_->head0_bias, error) ||
        !assign_tensor(tensors, "prediction_head.2.weight", shape2(1U, 8U),
                       &impl_->head2_weight, error) ||
        !assign_tensor(tensors, "prediction_head.2.bias", shape1(1U),
                       &impl_->head2_bias, error)) return false;
    std::ifstream scaler(scaler_path.c_str());
    if (!scaler) {
        if (error) *error = "cannot open SSE snapshot scaler: " + scaler_path;
        return false;
    }
    try {
        nlohmann::json payload;
        scaler >> payload;
        if (!payload["mean"].is_array() || !payload["scale"].is_array() ||
            payload["mean"].size() != feature_count ||
            payload["scale"].size() != feature_count) {
            if (error) *error = "snapshot scaler shape mismatch";
            return false;
        }
        impl_->mean.resize(feature_count);
        impl_->scale.resize(feature_count);
        for (std::size_t i = 0; i < feature_count; ++i) {
            impl_->mean[i] = payload["mean"][i].get<float>();
            impl_->scale[i] = payload["scale"][i].get<float>();
            if (!std::isfinite(impl_->mean[i]) ||
                !std::isfinite(impl_->scale[i]) || impl_->scale[i] <= 0.0f) {
                if (error) *error = "invalid snapshot scaler value";
                return false;
            }
        }
    } catch (const std::exception& ex) {
        if (error) *error = std::string("snapshot scaler parse failed: ") + ex.what();
        return false;
    }
    impl_->feature_count = feature_count;
    impl_->loaded = true;
    return true;
}

bool Model::loaded() const { return impl_.get() != 0 && impl_->loaded; }
std::size_t Model::feature_count() const {
    return impl_.get() == 0 ? 0U : impl_->feature_count;
}

bool Model::predict(const std::vector<float>& raw, State* state,
                    float* prediction, std::string* error) const {
    if (error) error->clear();
    if (!loaded() || state == 0 || prediction == 0 ||
        raw.size() != impl_->feature_count) {
        if (error) *error = "invalid SSE snapshot GRU model state or feature count";
        return false;
    }
    std::array<float, 95U> input_storage;
    input_storage.fill(0.0f);
    float* input = input_storage.data();
    for (std::size_t i = 0; i < impl_->feature_count; ++i) {
        if (std::isinf(raw[i])) {
            if (error) *error = "infinite raw snapshot factor";
            return false;
        }
        input[i] = (raw[i] - impl_->mean[i]) / impl_->scale[i];
        if (!std::isfinite(input[i])) input[i] = 0.0f;
    }

    std::array<float, 128U> projected;
    std::array<float, 512U> layer0;
    std::array<float, 256U> layer1;
    std::array<float, 128U> nonlinear;
    std::array<float, 128U> feature;
    matvec(impl_->f0_weight.values, 512U, impl_->feature_count,
           input, impl_->f0_bias.values.data(), layer0.data());
    for (std::size_t i = 0; i < layer0.size(); ++i) layer0[i] = softsign(layer0[i]);
    matvec(impl_->f2_weight.values, 256U, 512U,
           layer0.data(), impl_->f2_bias.values.data(), layer1.data());
    for (std::size_t i = 0; i < layer1.size(); ++i) layer1[i] = softsign(layer1[i]);
    matvec(impl_->f4_weight.values, 128U, 256U,
           layer1.data(), impl_->f4_bias.values.data(), nonlinear.data());
    for (std::size_t i = 0; i < nonlinear.size(); ++i) nonlinear[i] = softsign(nonlinear[i]);
    matvec(impl_->proj_weight.values, 128U, impl_->feature_count,
           input, impl_->proj_bias.values.data(), projected.data());
    for (std::size_t i = 0; i < feature.size(); ++i) feature[i] = nonlinear[i] + projected[i];

    std::array<float, 192U> input_gates;
    std::array<float, 192U> hidden_gates;
    matvec(impl_->gru_ih.values, 192U, 128U, feature.data(),
           impl_->gru_bih.values.data(), input_gates.data());
    matvec(impl_->gru_hh.values, 192U, 64U, state->hidden.data(),
           impl_->gru_bhh.values.data(), hidden_gates.data());
    std::array<float, kHiddenSize> recurrent;
    for (std::size_t i = 0; i < kHiddenSize; ++i) {
        const float reset = sigmoid(input_gates[i] + hidden_gates[i]);
        const float update = sigmoid(input_gates[64U + i] + hidden_gates[64U + i]);
        const float candidate = std::tanh(input_gates[128U + i] +
                                          reset * hidden_gates[128U + i]);
        recurrent[i] = candidate + update * (state->hidden[i] - candidate);
    }
    std::array<float, kHiddenSize> residual;
    matvec(impl_->res_weight.values, 64U, 128U, feature.data(),
           impl_->res_bias.values.data(), residual.data());
    std::array<float, kHiddenSize> encoded;
    float mean = 0.0f;
    for (std::size_t i = 0; i < kHiddenSize; ++i) {
        encoded[i] = recurrent[i] + residual[i];
        mean += encoded[i];
    }
    mean /= static_cast<float>(kHiddenSize);
    float variance = 0.0f;
    for (std::size_t i = 0; i < kHiddenSize; ++i) {
        const float centered = encoded[i] - mean;
        variance += centered * centered;
    }
    variance /= static_cast<float>(kHiddenSize);
    const float inverse_std = 1.0f / std::sqrt(variance + 1.0e-5f);
    for (std::size_t i = 0; i < kHiddenSize; ++i) {
        encoded[i] = (encoded[i] - mean) * inverse_std * impl_->ln_weight.values[i] +
                     impl_->ln_bias.values[i];
    }
    std::array<float, 8U> head;
    matvec(impl_->head0_weight.values, 8U, 64U, encoded.data(),
           impl_->head0_bias.values.data(), head.data());
    for (std::size_t i = 0; i < head.size(); ++i) head[i] = softsign(head[i]);
    float value = 0.0f;
    matvec(impl_->head2_weight.values, 1U, 8U, head.data(),
           impl_->head2_bias.values.data(), &value);
    if (!std::isfinite(value)) {
        if (error) *error = "non-finite SSE snapshot prediction";
        return false;
    }
    state->hidden = recurrent;
    ++state->accepted_rows;
    *prediction = value;
    return true;
}

}  // namespace sse_snapshot_gru
