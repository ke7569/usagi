#ifndef SSE_V06_MODEL_H
#define SSE_V06_MODEL_H
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sse_v06 {
typedef std::array<float, 50> Factors;
typedef std::array<float, 4> Heads;
struct State {
    std::array<float, 256> hidden;
    std::uint64_t rows;
    State() { reset(); }
    void reset() { hidden.fill(0); rows = 0; }
};
struct Stages {
    Factors normalized;
    std::array<float,128> projected;
};
class Model {
public:
    Model() : ready_(false) {}
    bool load(const std::string& path, std::string* error);
    bool predict(const Factors& input, State* state, Heads* heads, Stages* stages = 0) const;
    bool loaded() const { return ready_; }
private:
    std::vector<float> tensors_[14];
    bool ready_;
};
// Input is already compressed and in the exact v06 factor order.
int agreement(const Heads& heads);
const char* factor_name(std::size_t index);
Factors from_legacy_factors(const Factors& values);
}
#endif
