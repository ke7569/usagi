#include "snapshot_ensemble.h"

#include <cmath>

namespace sse_snapshot_gru {
namespace {

const std::uint64_t kOpen = 34200000000ULL;   // 09:30:00.000000
const std::uint64_t k0931 = 34260000000ULL;  // 09:31:00.000000
const std::uint64_t k0934 = 34440000000ULL;  // 09:34:00.000000
const std::uint64_t kClose = 34860000000ULL; // 09:40:00.000000 generation close

bool finite(float value) { return std::isfinite(value); }

}  // namespace

void DualState::reset() {
    baseline.reset();
    auction59.reset();
}

Prediction::Prediction()
    : baseline_pred(0.0f), auction59_pred(0.0f), baseline_weight(0.0f),
      auction59_weight(0.0f), ensemble_pred(0.0f), valid(false) {}

Ensemble::Ensemble() {}

bool Ensemble::load(const std::string& baseline_weights,
                    const std::string& baseline_scaler,
                    const std::string& auction_weights,
                    const std::string& auction_scaler,
                    std::string* error) {
    if (error) error->clear();
    if (!baseline_.load(baseline_weights, baseline_scaler, 36U, error)) return false;
    if (!auction59_.load(auction_weights, auction_scaler, 95U, error)) return false;
    return true;
}

bool Ensemble::loaded() const {
    return baseline_.loaded() && auction59_.loaded() &&
           baseline_.feature_count() == 36U && auction59_.feature_count() == 95U;
}

bool Ensemble::route(std::uint64_t time_of_day_micros,
                     float* baseline_weight, float* auction59_weight) {
    if (baseline_weight == 0 || auction59_weight == 0) return false;
    if (time_of_day_micros >= kOpen && time_of_day_micros < k0931) {
        *baseline_weight = 0.25f; *auction59_weight = 0.75f; return true;
    }
    if (time_of_day_micros >= k0931 && time_of_day_micros < k0934) {
        *baseline_weight = 0.50f; *auction59_weight = 0.50f; return true;
    }
    if (time_of_day_micros >= k0934 && time_of_day_micros < kClose) {
        *baseline_weight = 0.75f; *auction59_weight = 0.25f; return true;
    }
    return false;
}

bool Ensemble::predict(const std::vector<float>& snapshot36,
                       const std::vector<float>& snapshot_plus_auction59,
                       const std::string& exchange,
                       std::uint64_t time_of_day_micros,
                       DualState* state,
                       Prediction* output,
                       std::string* error) const {
    if (error) error->clear();
    if (!loaded() || state == 0 || output == 0 || exchange != "sse" ||
        snapshot36.size() != 36U || snapshot_plus_auction59.size() != 95U) {
        if (error) *error = "SSE opening ensemble input/contract mismatch";
        return false;
    }
    float baseline_weight = 0.0f, auction59_weight = 0.0f;
    if (!route(time_of_day_micros, &baseline_weight, &auction59_weight)) {
        if (error) *error = "SSE opening ensemble time outside [09:30,09:40)";
        return false;
    }

    // Run both arms against copies and commit hidden state only when both
    // predictions succeed.  This prevents a malformed Auction sidecar from
    // advancing the baseline stream by one row.
    State baseline_state = state->baseline;
    State auction_state = state->auction59;
    float baseline_pred = 0.0f, auction_pred = 0.0f;
    if (!baseline_.predict(snapshot36, &baseline_state, &baseline_pred, error) ||
        !auction59_.predict(snapshot_plus_auction59, &auction_state, &auction_pred, error) ||
        !finite(baseline_pred) || !finite(auction_pred)) {
        if (error && error->empty()) *error = "non-finite SSE arm prediction";
        return false;
    }
    const double blended = static_cast<double>(baseline_weight) *
                               static_cast<double>(baseline_pred) +
                           static_cast<double>(auction59_weight) *
                               static_cast<double>(auction_pred);
    const float ensemble_pred = static_cast<float>(blended);
    if (!finite(ensemble_pred)) {
        if (error) *error = "non-finite SSE ensemble prediction";
        return false;
    }
    state->baseline = baseline_state;
    state->auction59 = auction_state;
    output->baseline_pred = baseline_pred;
    output->auction59_pred = auction_pred;
    output->baseline_weight = baseline_weight;
    output->auction59_weight = auction59_weight;
    output->ensemble_pred = ensemble_pred;
    output->valid = true;
    return true;
}

}  // namespace sse_snapshot_gru
