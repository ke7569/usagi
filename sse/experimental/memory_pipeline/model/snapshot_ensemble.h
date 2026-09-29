#ifndef SSE_T0_SNAPSHOT_ENSEMBLE_H
#define SSE_T0_SNAPSHOT_ENSEMBLE_H

#include <cstdint>
#include <string>
#include <vector>

#include "snapshot_gru_runtime.h"

namespace sse_snapshot_gru {

struct DualState {
    State baseline;
    State auction59;

    void reset();
};

struct Prediction {
    float baseline_pred;
    float auction59_pred;
    float baseline_weight;
    float auction59_weight;
    float ensemble_pred;
    bool valid;

    Prediction();
};

// Candidate SSE opening policy from the 2026-08-17 model handoff.
// Exchange time is microseconds since midnight in Asia/Shanghai.  The
// policy is deliberately fail-closed and has no single-arm fallback.
class Ensemble {
public:
    Ensemble();

    bool load(const std::string& baseline_weights,
              const std::string& baseline_scaler,
              const std::string& auction_weights,
              const std::string& auction_scaler,
              std::string* error = 0);
    bool loaded() const;

    bool predict(const std::vector<float>& snapshot36,
                 const std::vector<float>& snapshot_plus_auction59,
                 const std::string& exchange,
                 std::uint64_t time_of_day_micros,
                 DualState* state,
                 Prediction* output,
                 std::string* error = 0) const;

    static bool route(std::uint64_t time_of_day_micros,
                      float* baseline_weight,
                      float* auction59_weight);

private:
    Model baseline_;
    Model auction59_;
};

}  // namespace sse_snapshot_gru

#endif  // SSE_T0_SNAPSHOT_ENSEMBLE_H
