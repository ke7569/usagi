#include "sse/model/sse_hybrid_model.h"

#include <cmath>

namespace sse_hybrid_model {
namespace {

const std::uint64_t kSseOpenMicros = 34200000000ULL;
const std::uint64_t kDayEndMicros = 86400000000ULL;

bool finite(float value) { return std::isfinite(value); }

}  // namespace

State::State() { reset(); }
void State::reset() {
    tick.reset();
    snapshot.reset();
}

Prediction::Prediction()
    : tick_generated(false), snapshot_generated(false), selected(false),
      selected_source(kNoSource), tick_pred(0.0f), snapshot_pred(0.0f),
      selected_pred(0.0f) {}

Model::Model() : loaded_(false) {}

bool Model::load(const std::string& tick_artifact,
                 const std::string& snapshot_baseline_artifact,
                 const std::string& snapshot_baseline_scaler,
                 const std::string& snapshot_auction_artifact,
                 const std::string& snapshot_auction_scaler,
                 std::string* error) {
    loaded_ = false;
    if (error) error->clear();
    if (!tick_.load(tick_artifact, error)) return false;
    if (!snapshot_.load(snapshot_baseline_artifact, snapshot_baseline_scaler,
                        snapshot_auction_artifact, snapshot_auction_scaler,
                        error)) return false;
    loaded_ = true;
    return true;
}

Source Model::selected_source(const std::string& exchange,
                              std::uint64_t time_of_day_micros) {
    if (exchange != "sse") return kNoSource;
    if (time_of_day_micros >= kSseOpenMicros &&
        time_of_day_micros < kSnapshotToTickSwitchMicros) return kSnapshotSource;
    if (time_of_day_micros >= kSnapshotToTickSwitchMicros &&
        time_of_day_micros < kDayEndMicros) return kTickSource;
    return kNoSource;
}

bool Model::on_tick(const std::array<float, sse_model::kFeatureCount>& factors,
                    const std::string& exchange,
                    std::uint64_t time_of_day_micros,
                    State* state,
                    Prediction* output,
                    std::string* error) const {
    if (error) error->clear();
    if (!loaded_ || exchange != "sse" || state == 0 || output == 0 ||
        time_of_day_micros >= kDayEndMicros) {
        if (error) *error = "invalid SSE hybrid tick input";
        return false;
    }
    *output = Prediction();
    float prediction = 0.0f;
    if (!tick_.predict(factors, &state->tick, &prediction) || !finite(prediction)) {
        if (error) *error = "SSE tick model rejected factor row";
        return false;
    }
    output->tick_generated = true;
    output->tick_pred = prediction;
    if (selected_source(exchange, time_of_day_micros) == kTickSource) {
        output->selected = true;
        output->selected_source = kTickSource;
        output->selected_pred = prediction;
    }
    return true;
}

bool Model::on_snapshot(const std::vector<float>& snapshot36,
                        const std::vector<float>& snapshot_plus_auction59,
                        const std::string& exchange,
                        std::uint64_t time_of_day_micros,
                        State* state,
                        Prediction* output,
                        std::string* error) const {
    if (error) error->clear();
    // Preserve the reviewed SSE branch's overlap window; selection still
    // switches to tick at 09:35, independently of snapshot generation.
    const std::uint64_t kSnapshotGenerationCloseMicros = 34860000000ULL;
    if (!loaded_ || exchange != "sse" || state == 0 || output == 0 ||
        time_of_day_micros < kSseOpenMicros ||
        time_of_day_micros >= kSnapshotGenerationCloseMicros) {
        if (error) *error = "SSE Snapshot row is outside the active opening window";
        return false;
    }
    *output = Prediction();
    sse_snapshot_gru::Prediction snapshot_prediction;
    if (!snapshot_.predict(snapshot36, snapshot_plus_auction59, exchange,
                           time_of_day_micros, &state->snapshot,
                           &snapshot_prediction, error) ||
        !snapshot_prediction.valid || !finite(snapshot_prediction.ensemble_pred)) {
        if (error && error->empty()) *error = "SSE Snapshot model rejected factor row";
        return false;
    }
    output->snapshot_generated = true;
    output->snapshot_pred = snapshot_prediction.ensemble_pred;
    if (selected_source(exchange, time_of_day_micros) == kSnapshotSource) {
        output->selected = true;
        output->selected_source = kSnapshotSource;
        output->selected_pred = snapshot_prediction.ensemble_pred;
    }
    return true;
}

}  // namespace sse_hybrid_model
