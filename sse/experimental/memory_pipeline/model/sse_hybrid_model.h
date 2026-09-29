#ifndef SSE_T0_HYBRID_MODEL_H
#define SSE_T0_HYBRID_MODEL_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "snapshot_ensemble.h"
#include "sse_model_runtime.h"

namespace sse_hybrid_model {

static const std::uint64_t kSnapshotToTickSwitchMicros = 34500000000ULL;

enum Source {
    kNoSource = 0,
    kSnapshotSource = 1,
    kTickSource = 2
};

struct State {
    sse_model::State tick;
    sse_snapshot_gru::DualState snapshot;

    State();
    void reset();
};

struct Prediction {
    bool tick_generated;
    bool snapshot_generated;
    bool selected;
    Source selected_source;
    float tick_pred;
    float snapshot_pred;
    float selected_pred;

    Prediction();
};

// Runs both SSE model families on their respective accepted event streams.
// Tick rows are accepted from the start of the SSE session so the recurrent
// state is warm when the 09:35 handover occurs. Snapshot rows are accepted only
// inside the packaged [09:30,09:35) Snapshot policy window. The selected
// output changes source exactly at 09:35:00, using exchange event time.
class Model {
public:
    Model();

    bool load(const std::string& tick_artifact,
              const std::string& snapshot_baseline_artifact,
              const std::string& snapshot_baseline_scaler,
              const std::string& snapshot_auction_artifact,
              const std::string& snapshot_auction_scaler,
              std::string* error = 0);
    bool loaded() const { return loaded_; }

    bool on_tick(const std::array<float, sse_model::kFeatureCount>& factors,
                 const std::string& exchange,
                 std::uint64_t time_of_day_micros,
                 State* state,
                 Prediction* output,
                 std::string* error = 0) const;

    bool on_snapshot(const std::vector<float>& snapshot36,
                     const std::vector<float>& snapshot_plus_auction59,
                     const std::string& exchange,
                     std::uint64_t time_of_day_micros,
                     State* state,
                     Prediction* output,
                     std::string* error = 0) const;

    static Source selected_source(const std::string& exchange,
                                  std::uint64_t time_of_day_micros);

private:
    sse_model::Model tick_;
    sse_snapshot_gru::Ensemble snapshot_;
    bool loaded_;
};

}  // namespace sse_hybrid_model

#endif  // SSE_T0_HYBRID_MODEL_H
