#ifndef SSE_T0_HYBRID_MODEL_H
#define SSE_T0_HYBRID_MODEL_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sse/model/snapshot_ensemble.h"
#include "common/model/legacy_midmix/sse_model_runtime.h"
#include "sse/model/v06_model.h"
#include "sse/model/v06_audit.h"

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
    sse_v06::State v06;

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
    bool multi_head;
    sse_v06::Heads heads;

    Prediction();
};

// Runs both SSE model families on their respective accepted event streams.
// Tick rows are accepted from the start of the SSE session so the recurrent
// state is warm when the 09:35 handover occurs. Snapshot rows are accepted only
// inside the packaged [09:30,09:41) Snapshot generation window. The selected
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
    bool load_v06(const std::string& path, std::string* error);
    bool is_v06() const { return v06_.loaded(); }

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
    sse_v06::Model v06_;
    bool loaded_;
};

}  // namespace sse_hybrid_model

#endif  // SSE_T0_HYBRID_MODEL_H
