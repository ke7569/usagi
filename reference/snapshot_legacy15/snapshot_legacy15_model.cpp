#include "snapshot_legacy15_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include "third_party/eigen3/Eigen/Dense"
#include "third_party/nlohmann/json.hpp"

namespace sze_snapshot15 {
namespace {
const char kMagic[8] = {'S','Z','E','1','5','G','R','U'};
const std::uint32_t kVersion = 1;

template <typename T>
bool read_exact(std::ifstream* in, T* value) {
    return static_cast<bool>(in->read(reinterpret_cast<char*>(value), sizeof(T)));
}

bool read_bytes(std::ifstream* in, void* value, std::size_t count) {
    return static_cast<bool>(in->read(static_cast<char*>(value), count));
}

bool assign_tensor(const std::vector<std::pair<std::string, Model::Tensor> >& all,
                  const char* name, Model::Tensor* out, std::string* error) {
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].first == name) { *out = all[i].second; return true; }
    }
    if (error) *error = std::string("missing tensor: ") + name;
    return false;
}
}

State::State() { reset(); }
void State::reset() { hidden.fill(0.0f); }

Model::Model() {}

bool Model::read_tensor(std::ifstream* in, std::string* name, Tensor* tensor,
                        std::string* error) {
    std::uint16_t name_len = 0;
    std::uint8_t rank = 0;
    std::uint8_t reserved[3];
    std::uint64_t byte_count = 0;
    if (!read_exact(in, &name_len) || name_len == 0 || name_len > 255) {
        if (error) *error = "invalid tensor name length"; return false;
    }
    name->assign(name_len, '\0');
    if (!read_bytes(in, &(*name)[0], name_len) || !read_exact(in, &rank) ||
        rank == 0 || rank > 4 || !read_bytes(in, reserved, sizeof(reserved))) {
        if (error) *error = "truncated tensor header"; return false;
    }
    tensor->shape.resize(rank);
    std::uint64_t elements = 1;
    for (std::size_t i = 0; i < rank; ++i) {
        if (!read_exact(in, &tensor->shape[i]) || tensor->shape[i] == 0 ||
            elements > 100000000ULL / tensor->shape[i]) {
            if (error) *error = "invalid tensor shape: " + *name; return false;
        }
        elements *= tensor->shape[i];
    }
    if (!read_exact(in, &byte_count) || byte_count != elements * sizeof(float) ||
        elements > 100000000ULL) {
        if (error) *error = "invalid tensor bytes: " + *name; return false;
    }
    tensor->values.resize(static_cast<std::size_t>(elements));
    if (!read_bytes(in, tensor->values.data(), tensor->values.size() * sizeof(float))) {
        if (error) *error = "truncated tensor: " + *name; return false;
    }
    return finite_vector(tensor->values);
}

bool Model::load(const std::string& weights_path, const std::string& scaler_path,
                 std::string* error) {
    loaded_ = false;
    std::ifstream in(weights_path.c_str(), std::ios::binary);
    if (!in) { if (error) *error = "cannot open weights: " + weights_path; return false; }
    char magic[8]; std::uint32_t version = 0, features = 0, hidden = 0, count = 0;
    if (!read_bytes(&in, magic, sizeof(magic)) || !read_exact(&in, &version) ||
        !read_exact(&in, &features) || !read_exact(&in, &hidden) || !read_exact(&in, &count) ||
        std::memcmp(magic, kMagic, sizeof(magic)) != 0 || version != kVersion ||
        features != 36 || hidden != 64 || count != 20) {
        if (error) *error = "weights header mismatch"; return false;
    }
    std::vector<std::pair<std::string, Tensor> > tensors;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string name; Tensor tensor;
        if (!read_tensor(&in, &name, &tensor, error)) return false;
        tensors.push_back(std::make_pair(name, tensor));
    }
#define GET(field, key) if (!assign_tensor(tensors, key, &field, error)) return false
    GET(proj_weight_, "proj.weight"); GET(proj_bias_, "proj.bias");
    GET(f0_weight_, "feature_layers.0.weight"); GET(f0_bias_, "feature_layers.0.bias");
    GET(f2_weight_, "feature_layers.2.weight"); GET(f2_bias_, "feature_layers.2.bias");
    GET(f4_weight_, "feature_layers.4.weight"); GET(f4_bias_, "feature_layers.4.bias");
    GET(gru_ih_, "gru.weight_ih_l0"); GET(gru_hh_, "gru.weight_hh_l0");
    GET(gru_bih_, "gru.bias_ih_l0"); GET(gru_bhh_, "gru.bias_hh_l0");
    GET(res_weight_, "res_gru.weight"); GET(res_bias_, "res_gru.bias");
    GET(ln_weight_, "ln.weight"); GET(ln_bias_, "ln.bias");
    GET(head0_weight_, "prediction_head.0.weight"); GET(head0_bias_, "prediction_head.0.bias");
    GET(head2_weight_, "prediction_head.2.weight"); GET(head2_bias_, "prediction_head.2.bias");
#undef GET
    std::ifstream scaler(scaler_path.c_str());
    if (!scaler) { if (error) *error = "cannot open scaler: " + scaler_path; return false; }
    try {
        nlohmann::json payload; scaler >> payload;
        if (!payload["mean"].is_array() || !payload["scale"].is_array() ||
            payload["mean"].size() != 36 || payload["scale"].size() != 36) {
            if (error) *error = "scaler shape mismatch"; return false;
        }
        for (std::size_t i = 0; i < 36; ++i) {
            mean_[i] = payload["mean"][i].get<float>();
            scale_[i] = payload["scale"][i].get<float>();
            if (!(scale_[i] > 0.0f) || !std::isfinite(mean_[i]) || !std::isfinite(scale_[i])) {
                if (error) *error = "invalid scaler value"; return false;
            }
        }
    } catch (const std::exception& ex) {
        if (error) *error = std::string("scaler parse failed: ") + ex.what(); return false;
    }
    loaded_ = true;
    return true;
}

float Model::softsign(float value) { return value / (1.0f + std::fabs(value)); }
float Model::sigmoid(float value) {
    if (value >= 0.0f) { const float e = std::exp(-value); return 1.0f / (1.0f + e); }
    const float e = std::exp(value); return e / (1.0f + e);
}
bool Model::finite_array(const std::array<float, 36>& values) {
    for (float value : values) if (!std::isfinite(value)) return false;
    return true;
}
bool Model::finite_vector(const std::vector<float>& values) {
    for (float value : values) if (!std::isfinite(value)) return false;
    return true;
}
bool Model::predict(const std::array<float, 36>& raw, State* state, float* output,
                    std::string* error) const {
    if (!loaded_ || !state || !output || !finite_array(raw)) {
        if (error) *error = "invalid model state or raw factors"; return false;
    }
    std::array<float, 36> x;
    for (std::size_t i = 0; i < 36; ++i) {
        x[i] = (raw[i] - mean_[i]) / scale_[i];
        if (!std::isfinite(x[i])) x[i] = 0.0f;
    }
    typedef Eigen::Matrix<float, 36, 1> Vector36;
    typedef Eigen::Matrix<float, 64, 1> Vector64;
    typedef Eigen::Matrix<float, 8, 1> Vector8;
    typedef Eigen::Matrix<float, 128, 1> Vector128;
    typedef Eigen::Matrix<float, 192, 1> Vector192;
    typedef Eigen::Matrix<float, 256, 1> Vector256;
    typedef Eigen::Matrix<float, 512, 1> Vector512;
    typedef Eigen::Matrix<float, 128, 36, Eigen::RowMajor> Matrix128x36;
    typedef Eigen::Matrix<float, 512, 36, Eigen::RowMajor> Matrix512x36;
    typedef Eigen::Matrix<float, 256, 512, Eigen::RowMajor> Matrix256x512;
    typedef Eigen::Matrix<float, 128, 256, Eigen::RowMajor> Matrix128x256;
    typedef Eigen::Matrix<float, 192, 128, Eigen::RowMajor> Matrix192x128;
    typedef Eigen::Matrix<float, 192, 64, Eigen::RowMajor> Matrix192x64;
    typedef Eigen::Matrix<float, 64, 128, Eigen::RowMajor> Matrix64x128;
    typedef Eigen::Matrix<float, 8, 64, Eigen::RowMajor> Matrix8x64;

    const Eigen::Map<const Matrix128x36> proj_weight(proj_weight_.values.data());
    const Eigen::Map<const Matrix512x36> f0_weight(f0_weight_.values.data());
    const Eigen::Map<const Matrix256x512> f2_weight(f2_weight_.values.data());
    const Eigen::Map<const Matrix128x256> f4_weight(f4_weight_.values.data());
    const Eigen::Map<const Vector36> x_map(x.data());
    const Eigen::Map<const Vector128> proj_bias(proj_bias_.values.data());
    const Eigen::Map<const Vector512> f0_bias(f0_bias_.values.data());
    const Eigen::Map<const Vector256> f2_bias(f2_bias_.values.data());
    const Eigen::Map<const Vector128> f4_bias(f4_bias_.values.data());

    Vector128 projected;
    projected.noalias() = proj_weight * x_map;
    projected += proj_bias;
    Vector512 a;
    a.noalias() = f0_weight * x_map;
    a += f0_bias;
    for (Eigen::Index i = 0; i < a.size(); ++i) a(i) = softsign(a(i));
    Vector256 tmp256;
    tmp256.noalias() = f2_weight * a;
    tmp256 += f2_bias;
    for (Eigen::Index i = 0; i < tmp256.size(); ++i) tmp256(i) = softsign(tmp256(i));
    Vector128 nonlinear;
    nonlinear.noalias() = f4_weight * tmp256;
    nonlinear += f4_bias;
    for (Eigen::Index i = 0; i < nonlinear.size(); ++i) nonlinear(i) = softsign(nonlinear(i));
    Vector128 feature = projected + nonlinear;

    std::array<float, 64> next;
    const Eigen::Map<const Matrix192x128> gru_ih(gru_ih_.values.data());
    const Eigen::Map<const Matrix192x64> gru_hh(gru_hh_.values.data());
    const Eigen::Map<const Vector192> gru_bih(gru_bih_.values.data());
    const Eigen::Map<const Vector192> gru_bhh(gru_bhh_.values.data());
    const Eigen::Map<const Vector64> previous(state->hidden.data());
    Vector192 input_gates;
    input_gates.noalias() = gru_ih * feature;
    input_gates += gru_bih;
    Vector192 hidden_gates;
    hidden_gates.noalias() = gru_hh * previous;
    hidden_gates += gru_bhh;
    for (std::size_t r = 0; r < 64; ++r) {
        const float rh = input_gates(static_cast<Eigen::Index>(r)) +
            hidden_gates(static_cast<Eigen::Index>(r));
        const float zh = input_gates(static_cast<Eigen::Index>(64 + r)) +
            hidden_gates(static_cast<Eigen::Index>(64 + r));
        const float reset = sigmoid(rh);
        const float update = sigmoid(zh);
        const float candidate = std::tanh(
            input_gates(static_cast<Eigen::Index>(128 + r)) +
            reset * hidden_gates(static_cast<Eigen::Index>(128 + r)));
        next[r] = (1.0f - update) * candidate + update * state->hidden[r];
    }
    std::array<float, 64> encoded;
    const Eigen::Map<const Matrix64x128> res_weight(res_weight_.values.data());
    const Eigen::Map<const Vector64> res_bias(res_bias_.values.data());
    const Eigen::Map<const Vector64> next_map(next.data());
    Vector64 encoded_vector;
    encoded_vector.noalias() = res_weight * feature;
    encoded_vector += next_map;
    encoded_vector += res_bias;
    float mean = 0.0f;
    for (std::size_t r = 0; r < 64; ++r) {
        encoded[r] = encoded_vector(static_cast<Eigen::Index>(r));
        mean += encoded[r];
    }
    mean /= 64.0f;
    float var = 0.0f;
    for (float value : encoded) { const float d = value - mean; var += d * d; }
    var /= 64.0f;
    const float inv = 1.0f / std::sqrt(var + 1.0e-5f);
    for (std::size_t r = 0; r < 64; ++r) encoded[r] = (encoded[r] - mean) * inv * ln_weight_.values[r] + ln_bias_.values[r];
    const Eigen::Map<const Matrix8x64> head0_weight(head0_weight_.values.data());
    const Eigen::Map<const Vector8> head0_bias(head0_bias_.values.data());
    const Eigen::Map<const Vector64> encoded_map(encoded.data());
    Vector8 head;
    head.noalias() = head0_weight * encoded_map;
    head += head0_bias;
    for (Eigen::Index i = 0; i < head.size(); ++i) head(i) = softsign(head(i));
    const Eigen::Map<const Vector8> head2_weight(head2_weight_.values.data());
    const float value = head2_bias_.values[0] + head2_weight.dot(head);
    if (!std::isfinite(value)) { if (error) *error = "non-finite prediction"; return false; }
    state->hidden = next;
    *output = value;
    return true;
}
}  // namespace sze_snapshot15
