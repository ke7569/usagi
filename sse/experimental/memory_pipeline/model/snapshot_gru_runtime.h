#ifndef SSE_T0_SNAPSHOT_GRU_RUNTIME_H
#define SSE_T0_SNAPSHOT_GRU_RUNTIME_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sse_snapshot_gru {

static const std::size_t kHiddenSize = 64U;

struct State {
    std::array<float, kHiddenSize> hidden;
    std::uint64_t accepted_rows;

    State();
    void reset();
};

// Native CPU implementation of the handoff's legacy-residual-gru topology.
// The artifact is produced offline by convert_snapshot_gru.py.  The serving
// process does not link PyTorch, TorchScript, or any Python runtime.
class Model {
public:
    Model();
    ~Model();

    Model(Model&& other) noexcept;
    Model& operator=(Model&& other) noexcept;

    bool load(const std::string& weights_path,
              const std::string& scaler_path,
              std::size_t expected_feature_count,
              std::string* error = 0);
    bool loaded() const;
    std::size_t feature_count() const;

    // NaN raw factors follow the handoff's mean-impute-then-zscore contract
    // and become zero after normalization.  Infinity is rejected.
    bool predict(const std::vector<float>& raw,
                 State* state,
                 float* prediction,
                 std::string* error = 0) const;

private:
    Model(const Model&);
    Model& operator=(const Model&);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sse_snapshot_gru

#endif  // SSE_T0_SNAPSHOT_GRU_RUNTIME_H
