#ifndef T0_PREDICTOR_MIX153060_MODEL_H
#define T0_PREDICTOR_MIX153060_MODEL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mix153060 {

static const std::size_t kFeatureCount = 50;
static const std::size_t kHiddenSize = 128;
static const std::size_t kLayerCount = 2;

const std::array<const char*, kFeatureCount>& factor_names();

struct State {
    std::array<float, kHiddenSize * kLayerCount> hidden;
    std::uint64_t accepted_rows;

    State();
    void reset();
};

struct Trace {
    std::array<float, kFeatureCount> layernorm_output;
    std::array<float, kHiddenSize> projected_input;
    std::array<float, kHiddenSize> gru_output;

    Trace();
};

// Immutable weights are shared across instruments. State is deliberately
// supplied by the caller so each instrument-day owns an independent GRU
// sequence and can reset it at the trading-day boundary.
class Model {
public:
    Model();
    ~Model();

    Model(Model&& other) noexcept;
    Model& operator=(Model&& other) noexcept;

    bool load(const std::string& path, std::string* error);
    bool loaded() const;

    bool predict(const std::array<float, kFeatureCount>& factors,
                 State* state,
                 float* prediction,
                 Trace* trace = 0) const;

    const std::string& checkpoint_sha256() const;
    const std::string& factor_names_sha256() const;

private:
    Model(const Model&);
    Model& operator=(const Model&);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mix153060

#endif  // T0_PREDICTOR_MIX153060_MODEL_H
