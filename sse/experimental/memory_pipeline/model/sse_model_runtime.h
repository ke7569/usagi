#ifndef SSE_T0_MODEL_RUNTIME_H
#define SSE_T0_MODEL_RUNTIME_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sse_model {

static const std::size_t kFeatureCount = 50U;
static const std::size_t kHiddenSize = 64U;
static const char kArtifactMagic[] = "SSEMODL1";
static const char kFactorNamesSha256[] =
    "24fd61f8c498278dd67db7f183aa4846ae50078143bbd358958299f8817db089";

struct State {
    std::array<float, kHiddenSize> hidden;
    std::uint64_t accepted_rows;

    State();
    void reset();
};

// Native CPU inference for the supplied v04-legacy-midmix-sse topology.
// Weights are loaded from the SSEMODL1 artifact produced by
// convert_state_dict_npz.py; no PyTorch or TorchScript is linked at runtime.
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
                 float* prediction) const;

private:
    Model(const Model&);
    Model& operator=(const Model&);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sse_model

#endif  // SSE_T0_MODEL_RUNTIME_H
