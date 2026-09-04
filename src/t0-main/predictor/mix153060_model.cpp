#include "mix153060_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

#include <immintrin.h>

#include "../cpp_model/eigen3/Eigen/Dense"

namespace mix153060 {

namespace {

static const char kMagic[8] = {'M', 'I', 'X', '1', '5', '3', '0', '6'};
static const std::uint32_t kFormatVersion = 1;
static const std::uint32_t kEndianMarker = 0x01020304U;
static const std::size_t kGateCount = 3 * kHiddenSize;
static const float kExpectedLayerNormEpsilon = 1.0e-5f;
static const char kExpectedCheckpointHash[] =
    "09ed1cf8b824d75708faf725cf14797c3b0f32635dbad59374c04f4ff7fb3bb5";
static const char kExpectedFactorHash[] =
    "e20ed70098a025f597f8b9cda41fb79b3188d875ad227f243c872ddbfbbed97e";

typedef Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> RowMatrix;

static inline float dot_product(const float* left, const float* right,
                                std::size_t count) {
#if defined(__AVX2__)
    __m256 sum0 = _mm256_setzero_ps();
    __m256 sum1 = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(
            _mm256_loadu_ps(left + i), _mm256_loadu_ps(right + i)));
        sum1 = _mm256_add_ps(sum1, _mm256_mul_ps(
            _mm256_loadu_ps(left + i + 8), _mm256_loadu_ps(right + i + 8)));
    }
    sum0 = _mm256_add_ps(sum0, sum1);
    __m128 lo = _mm256_castps256_ps128(sum0);
    __m128 hi = _mm256_extractf128_ps(sum0, 1);
    __m128 sum = _mm_add_ps(lo, hi);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    float result = _mm_cvtss_f32(sum);
    for (; i < count; ++i) result += left[i] * right[i];
    return result;
#else
    float result = 0.0f;
    for (std::size_t i = 0; i < count; ++i) result += left[i] * right[i];
    return result;
#endif
}

static inline float row_dot(const RowMatrix& matrix, std::size_t row,
                            const float* input, std::size_t count) {
    return dot_product(matrix.data() + row * count, input, count);
}

struct Tensor {
    std::vector<std::uint32_t> shape;
    std::vector<float> values;
};

bool read_exact(std::istream* stream, void* destination, std::size_t size) {
    if (stream == 0 || destination == 0) {
        return false;
    }
    stream->read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return stream->good() || stream->gcount() == static_cast<std::streamsize>(size);
}

bool read_u8(std::istream* stream, std::uint8_t* value) {
    return read_exact(stream, value, sizeof(*value));
}

bool read_u16(std::istream* stream, std::uint16_t* value) {
    unsigned char bytes[2];
    if (!read_exact(stream, bytes, sizeof(bytes))) {
        return false;
    }
    *value = static_cast<std::uint16_t>(bytes[0]) |
             (static_cast<std::uint16_t>(bytes[1]) << 8);
    return true;
}

bool read_u32(std::istream* stream, std::uint32_t* value) {
    unsigned char bytes[4];
    if (!read_exact(stream, bytes, sizeof(bytes))) {
        return false;
    }
    *value = static_cast<std::uint32_t>(bytes[0]) |
             (static_cast<std::uint32_t>(bytes[1]) << 8) |
             (static_cast<std::uint32_t>(bytes[2]) << 16) |
             (static_cast<std::uint32_t>(bytes[3]) << 24);
    return true;
}

bool read_u64(std::istream* stream, std::uint64_t* value) {
    unsigned char bytes[8];
    if (!read_exact(stream, bytes, sizeof(bytes))) {
        return false;
    }
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
        result |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    *value = result;
    return true;
}

bool read_f32(std::istream* stream, float* value) {
    std::uint32_t bits = 0;
    if (!read_u32(stream, &bits)) {
        return false;
    }
    std::memcpy(value, &bits, sizeof(bits));
    return true;
}

bool read_string(std::istream* stream, std::string* value) {
    std::uint16_t size = 0;
    if (!read_u16(stream, &size)) {
        return false;
    }
    value->assign(size, '\0');
    return size == 0 || read_exact(stream, &(*value)[0], size);
}

std::string hex_string(const unsigned char* data, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        result[2 * i] = digits[data[i] >> 4];
        result[2 * i + 1] = digits[data[i] & 0x0f];
    }
    return result;
}

bool shape_is(const Tensor& tensor, std::uint32_t first) {
    return tensor.shape.size() == 1 && tensor.shape[0] == first;
}

bool shape_is(const Tensor& tensor, std::uint32_t first, std::uint32_t second) {
    return tensor.shape.size() == 2 && tensor.shape[0] == first && tensor.shape[1] == second;
}

bool copy_vector(const std::map<std::string, Tensor>& tensors,
                 const std::string& name,
                 std::uint32_t size,
                 Eigen::VectorXf* destination,
                 std::string* error) {
    const std::map<std::string, Tensor>::const_iterator found = tensors.find(name);
    if (found == tensors.end() || !shape_is(found->second, size)) {
        if (error != 0) {
            *error = name + " has an unexpected or missing shape";
        }
        return false;
    }
    *destination = Eigen::Map<const Eigen::VectorXf>(found->second.values.data(), size);
    return true;
}

bool copy_matrix(const std::map<std::string, Tensor>& tensors,
                 const std::string& name,
                 std::uint32_t rows,
                 std::uint32_t columns,
                 RowMatrix* destination,
                 std::string* error) {
    const std::map<std::string, Tensor>::const_iterator found = tensors.find(name);
    if (found == tensors.end() || !shape_is(found->second, rows, columns)) {
        if (error != 0) {
            *error = name + " has an unexpected or missing shape";
        }
        return false;
    }
    destination->resize(rows, columns);
    std::memcpy(destination->data(),
                found->second.values.data(),
                found->second.values.size() * sizeof(float));
    return true;
}

float sigmoid(float value) {
    return 1.0f / (1.0f + std::exp(-value));
}

template <typename Derived>
Eigen::VectorXf affine(const RowMatrix& weight,
                       const Eigen::MatrixBase<Derived>& input,
                       const Eigen::VectorXf& bias) {
    return weight * input + bias;
}

}  // namespace

const std::array<const char*, kFeatureCount>& factor_names() {
    static const std::array<const char*, kFeatureCount> names = {{
        "factor_spread_permille",
        "factor_mid_return_permille",
        "factor_weighted_return_permille_1",
        "factor_weighted_return_permille_2",
        "factor_weighted_return_permille_3",
        "factor_weighted_return_permille_4",
        "factor_weighted_return_permille_5",
        "factor_weighted_volume_imbalance",
        "factor_volume_imbalance",
        "factor_percent_turnover",
        "factor_liquidity_ask_l1_share",
        "factor_liquidity_bid_l1_share",
        "factor_hermes_permille",
        "factor_tr_sqrt_positive",
        "factor_fee_on_tick",
        "factor_bid_volume_change_ratio",
        "factor_ask_volume_change_ratio",
        "factor_weighted_ask_permille",
        "factor_weighted_bid_permille",
        "factor_weighted_ask_return_permille",
        "factor_weighted_bid_return_permille",
        "factor_positive_fill_rate",
        "factor_negative_fill_rate",
        "factor_order_flow_imbalance",
        "factor_cfr_imbalance",
        "factor_book_count_imbalance_l1",
        "factor_book_count_imbalance_l5",
        "factor_book_avg_size_imbalance_l1",
        "factor_book_avg_size_imbalance_l5",
        "factor_book_life_imbalance_l1",
        "factor_book_life_imbalance_l5",
        "factor_book_fixdist_imbalance_1pct",
        "factor_book_fixdist_imbalance_5pct",
        "factor_book_fixdist_weighted_1pct",
        "factor_book_fixdist_weighted_5pct",
        "factor_book_avg_size_imbalance",
        "factor_book_count_imbalance",
        "factor_book_life_imbalance",
        "factor_book_young_imbalance_1pct",
        "factor_max_bid_distance_ratio",
        "factor_max_ask_distance_ratio",
        "factor_max_vol_distance_imbalance",
        "factor_book_fixdist_hermes",
        "factor_positive_order_flow_log1p",
        "factor_negative_order_flow_log1p",
        "factor_market_flow_asinh",
        "factor_cancel_buy_flow_log1p",
        "factor_cancel_sell_flow_log1p",
        "factor_positive_trade_log1p",
        "factor_negative_trade_log1p",
    }};
    return names;
}

State::State() : hidden(), accepted_rows(0) {
    hidden.fill(0.0f);
}

void State::reset() {
    hidden.fill(0.0f);
    accepted_rows = 0;
}

Trace::Trace() : layernorm_output(), projected_input(), gru_output() {
    layernorm_output.fill(0.0f);
    projected_input.fill(0.0f);
    gru_output.fill(0.0f);
}

struct Model::Impl {
    float epsilon;
    Eigen::VectorXf norm_weight;
    Eigen::VectorXf norm_bias;
    RowMatrix projection_weight;
    Eigen::VectorXf projection_bias;
    std::array<RowMatrix, kLayerCount> weight_ih;
    std::array<RowMatrix, kLayerCount> weight_hh;
    std::array<Eigen::VectorXf, kLayerCount> bias_ih;
    std::array<Eigen::VectorXf, kLayerCount> bias_hh;
    RowMatrix head_weight;
    Eigen::VectorXf head_bias;
    std::string checkpoint_hash;
    std::string factor_hash;

    Impl() : epsilon(1.0e-5f) {}
};

Model::Model() : impl_() {}
Model::~Model() {}
Model::Model(Model&& other) noexcept : impl_(std::move(other.impl_)) {}
Model& Model::operator=(Model&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}

bool Model::load(const std::string& path, std::string* error) {
    if (error != 0) {
        error->clear();
    }
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input.is_open()) {
        if (error != 0) {
            *error = "cannot open model artifact: " + path;
        }
        return false;
    }

    char magic[8];
    std::uint32_t version = 0;
    std::uint32_t endian = 0;
    std::uint32_t features = 0;
    std::uint32_t hidden = 0;
    std::uint32_t layers = 0;
    std::uint32_t tensor_count = 0;
    float epsilon = 0.0f;
    std::uint32_t factor_count = 0;
    unsigned char checkpoint_hash[32];
    unsigned char factor_hash[32];
    if (!read_exact(&input, magic, sizeof(magic)) ||
        !read_u32(&input, &version) || !read_u32(&input, &endian) ||
        !read_u32(&input, &features) || !read_u32(&input, &hidden) ||
        !read_u32(&input, &layers) || !read_u32(&input, &tensor_count) ||
        !read_f32(&input, &epsilon) || !read_u32(&input, &factor_count) ||
        !read_exact(&input, checkpoint_hash, sizeof(checkpoint_hash)) ||
        !read_exact(&input, factor_hash, sizeof(factor_hash))) {
        if (error != 0) {
            *error = "truncated model header";
        }
        return false;
    }
    if (std::memcmp(magic, kMagic, sizeof(magic)) != 0 || version != kFormatVersion ||
        endian != kEndianMarker || features != kFeatureCount || hidden != kHiddenSize ||
        layers != kLayerCount || tensor_count != 14 || factor_count != kFeatureCount ||
        !std::isfinite(epsilon) || epsilon != kExpectedLayerNormEpsilon) {
        if (error != 0) {
            *error = "model header does not match mix153060 v1";
        }
        return false;
    }

    const std::array<const char*, kFeatureCount>& expected_names = factor_names();
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        std::string name;
        if (!read_string(&input, &name) || name != expected_names[i]) {
            if (error != 0) {
                std::ostringstream message;
                message << "factor contract mismatch at index " << i;
                *error = message.str();
            }
            return false;
        }
    }

    std::map<std::string, Tensor> tensors;
    for (std::uint32_t index = 0; index < tensor_count; ++index) {
        std::string name;
        std::uint8_t rank = 0;
        if (!read_string(&input, &name) || !read_u8(&input, &rank) || rank == 0 || rank > 4) {
            if (error != 0) {
                *error = "invalid tensor record";
            }
            return false;
        }
        Tensor tensor;
        std::uint64_t element_count = 1;
        for (std::uint8_t dimension = 0; dimension < rank; ++dimension) {
            std::uint32_t size = 0;
            if (!read_u32(&input, &size) || size == 0) {
                if (error != 0) {
                    *error = name + " has an invalid dimension";
                }
                return false;
            }
            if (element_count > 1000000ULL / size) {
                if (error != 0) {
                    *error = name + " is too large";
                }
                return false;
            }
            tensor.shape.push_back(size);
            element_count *= size;
        }
        std::uint64_t byte_count = 0;
        if (!read_u64(&input, &byte_count) || byte_count != element_count * sizeof(float) ||
            element_count > 1000000) {
            if (error != 0) {
                *error = name + " has an invalid byte count";
            }
            return false;
        }
        tensor.values.resize(static_cast<std::size_t>(element_count));
        for (std::size_t i = 0; i < tensor.values.size(); ++i) {
            if (!read_f32(&input, &tensor.values[i]) ||
                !std::isfinite(tensor.values[i])) {
                if (error != 0) {
                    *error = "truncated or non-finite tensor: " + name;
                }
                return false;
            }
        }
        if (!tensors.insert(std::make_pair(name, tensor)).second) {
            if (error != 0) {
                *error = "duplicate tensor: " + name;
            }
            return false;
        }
    }
    char trailing = 0;
    if (input.read(&trailing, 1)) {
        if (error != 0) {
            *error = "unexpected trailing bytes in model artifact";
        }
        return false;
    }

    const std::string embedded_checkpoint_hash =
        hex_string(checkpoint_hash, sizeof(checkpoint_hash));
    const std::string embedded_factor_hash = hex_string(factor_hash, sizeof(factor_hash));
    if (embedded_checkpoint_hash != kExpectedCheckpointHash) {
        if (error != 0) {
            *error = "model checkpoint hash does not match the accepted mix153060 artifact";
        }
        return false;
    }
    if (embedded_factor_hash != kExpectedFactorHash) {
        if (error != 0) {
            *error = "model factor hash does not match the compiled mix153060 contract";
        }
        return false;
    }
    std::unique_ptr<Impl> next(new Impl());
    next->epsilon = epsilon;
    next->checkpoint_hash = embedded_checkpoint_hash;
    next->factor_hash = embedded_factor_hash;
    if (!copy_vector(tensors, "input_norm.weight", kFeatureCount, &next->norm_weight, error) ||
        !copy_vector(tensors, "input_norm.bias", kFeatureCount, &next->norm_bias, error) ||
        !copy_matrix(tensors, "input_proj.weight", kHiddenSize, kFeatureCount,
                     &next->projection_weight, error) ||
        !copy_vector(tensors, "input_proj.bias", kHiddenSize, &next->projection_bias, error)) {
        return false;
    }
    for (std::size_t layer = 0; layer < kLayerCount; ++layer) {
        const std::string suffix = layer == 0 ? "0" : "1";
        if (!copy_matrix(tensors, "gru.weight_ih_l" + suffix, kGateCount, kHiddenSize,
                         &next->weight_ih[layer], error) ||
            !copy_matrix(tensors, "gru.weight_hh_l" + suffix, kGateCount, kHiddenSize,
                         &next->weight_hh[layer], error) ||
            !copy_vector(tensors, "gru.bias_ih_l" + suffix, kGateCount,
                         &next->bias_ih[layer], error) ||
            !copy_vector(tensors, "gru.bias_hh_l" + suffix, kGateCount,
                         &next->bias_hh[layer], error)) {
            return false;
        }
    }
    if (!copy_matrix(tensors, "head.weight", 1, kHiddenSize, &next->head_weight, error) ||
        !copy_vector(tensors, "head.bias", 1, &next->head_bias, error)) {
        return false;
    }
    impl_.swap(next);
    return true;
}

bool Model::loaded() const {
    return impl_.get() != 0;
}

bool Model::predict(const std::array<float, kFeatureCount>& factors,
                    State* state,
                    float* prediction,
                    Trace* trace) const {
    if (impl_.get() == 0 || state == 0 || prediction == 0) {
        return false;
    }
    float normalized[kFeatureCount];
    float layer_input[kHiddenSize];
    float input_gates[kGateCount];
    float hidden_gates[kGateCount];
    float next[kHiddenSize];
    float mean = 0.0f;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        if (!std::isfinite(factors[i])) {
            return false;
        }
        mean += factors[i];
    }
    mean /= static_cast<float>(kFeatureCount);
    float variance = 0.0f;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        const float centered = factors[i] - mean;
        variance += centered * centered;
    }
    variance /= static_cast<float>(kFeatureCount);
    const float inverse_std = 1.0f / std::sqrt(variance + impl_->epsilon);
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        normalized[i] = (factors[i] - mean) * inverse_std * impl_->norm_weight(i) +
                        impl_->norm_bias(i);
        if (trace != 0) {
            trace->layernorm_output[i] = normalized[i];
        }
    }

    for (std::size_t i = 0; i < kHiddenSize; ++i) {
        layer_input[i] = row_dot(impl_->projection_weight, i, normalized, kFeatureCount) +
                         impl_->projection_bias(i);
    }
    if (trace != 0) {
        std::copy(layer_input, layer_input + kHiddenSize, trace->projected_input.begin());
    }
    for (std::size_t layer = 0; layer < kLayerCount; ++layer) {
        float* previous = state->hidden.data() + layer * kHiddenSize;
        // Each GRU gate projection has three contiguous 128-wide gate
        // blocks (reset, update, candidate). Compute all 384 outputs before
        // consuming the three blocks below.
        for (std::size_t index = 0; index < kGateCount; ++index) {
            input_gates[index] = row_dot(impl_->weight_ih[layer], index,
                                         layer_input, kHiddenSize) +
                                 impl_->bias_ih[layer](index);
            hidden_gates[index] = row_dot(impl_->weight_hh[layer], index,
                                          previous, kHiddenSize) +
                                  impl_->bias_hh[layer](index);
        }
        for (std::size_t index = 0; index < kHiddenSize; ++index) {
            const float reset = sigmoid(input_gates[index] + hidden_gates[index]);
            const float update = sigmoid(
                input_gates[index + kHiddenSize] + hidden_gates[index + kHiddenSize]);
            const float candidate = std::tanh(
                input_gates[index + 2 * kHiddenSize] +
                reset * hidden_gates[index + 2 * kHiddenSize]);
            // This is PyTorch's native GRU update order: n + z * (h - n).
            // The algebraically equivalent (1-z)*n + z*h accumulates several
            // ULPs of drift over a full trading day.
            next[index] = candidate + update * (previous[index] - candidate);
        }
        std::copy(next, next + kHiddenSize, previous);
        std::copy(next, next + kHiddenSize, layer_input);
    }

    const float value = row_dot(impl_->head_weight, 0, layer_input, kHiddenSize) +
                        impl_->head_bias(0);
    if (!std::isfinite(value)) {
        return false;
    }
    ++state->accepted_rows;
    *prediction = value;
    if (trace != 0) {
        std::copy(layer_input, layer_input + kHiddenSize, trace->gru_output.begin());
    }
    return true;
}

const std::string& Model::checkpoint_sha256() const {
    static const std::string empty;
    return impl_.get() == 0 ? empty : impl_->checkpoint_hash;
}

const std::string& Model::factor_names_sha256() const {
    static const std::string empty;
    return impl_.get() == 0 ? empty : impl_->factor_hash;
}

}  // namespace mix153060
