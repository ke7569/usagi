#include "sse_model_consumer.h"
#include "../model/snapshot36.h"
#include <chrono>
#include <limits>

namespace sse_pipeline {
namespace {
std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
const std::uint64_t kSwitchUs = 34500000000ULL;
}
ModelConsumer::ModelConsumer(const ModelOptions& options) : options_(options) {}

bool ModelConsumer::load(std::string* error) {
    if (!sse_tick::load_daily_static_metadata(options_.static_json, options_.trading_day, &metadata_, error) ||
        !tick_.load(options_.tick_model, error)) return false;
    if (!options_.snapshot_enabled) return true;
    if (!options_.auction_csv.empty() && !sse_auction59::load_csv(options_.auction_csv, &auction_, error)) return false;
    return snapshot_.load(options_.snapshot_baseline, options_.snapshot_baseline_scaler,
                          options_.snapshot_auction, options_.snapshot_auction_scaler, error);
}

bool ModelConsumer::consume(const Event& event) {
    const std::string symbol(event.security_id);
    if (!initialized_.count(symbol)) {
        const auto found = metadata_.find(symbol);
        if (found == metadata_.end()) { ++stats_.missing_metadata; initialized_[symbol] = false; }
        else initialized_[symbol] = tick_.set_static_metadata(symbol, found->second);
    }
    if (event.kind == kTick) {
        ++stats_.ticks;
        sse_live::TickEvent tick;
        if (!sse_live::decode_primary_tick(event.payload, event.payload_size, &tick, &error_)) return false;
        sse_tick_strategy::PredictionRow row;
        const auto started = now_ns();
        const bool ok = tick_.observe(tick, event.receive_mono_ns / 1000, 0, &row, &error_);
        stats_.observe.observe(now_ns() - started);
        if (!ok) { ++stats_.tick_rejected; ++stats_.quality[error_]; }
        return ok;
    }
    if (event.kind == kTickSample) {
        if (event.exchange_time_us < options_.tick_start_us) return true;
        ++stats_.samples;
        sse_live::TickEvent tick;
        if (!sse_live::decode_primary_tick(event.payload, event.payload_size, &tick, &error_)) return false;
        sse_tick_strategy::PredictionRow row;
        const std::uint64_t boundary_us = event.receive_mono_ns / 1000;
        const bool ok = tick_.sample_tick(tick, boundary_us - 101, boundary_us, &row, &error_);
        ++stats_.quality[row.quality ? row.quality : "unknown"];
        if (row.factor_ns) stats_.factor.observe(row.factor_ns);
        if (row.infer_ns) stats_.infer.observe(row.infer_ns);
        if (!row.security_id.empty()) {
            ++stats_.predictions; ++stock_predictions_[symbol]; stats_.prediction_sum += row.prediction;
            if (row.selected && event.exchange_time_us >= kSwitchUs) ++stats_.selected;
            if (!row.model_valid) ++stats_.model_errors;
        }
        return ok;
    }
    if (event.kind != kSnapshot) return false;
    ++stats_.snapshots;
    sse_live::Snapshot value;
    if (!sse_live::decode_primary_snapshot(event.payload, event.payload_size, &value, &error_)) return false;
    tick_.observe_snapshot(value, &error_);
    if (!options_.snapshot_enabled || value.time_of_day_micros < 34200000000ULL || value.time_of_day_micros >= kSwitchUs) return true;
    SnapshotState& state = snapshots_[symbol];
    if (!sse_snapshot36::valid(value) || (state.previous_valid &&
        (value.time_of_day_micros <= state.previous.time_of_day_micros || value.volume < state.previous.volume || value.turnover < state.previous.turnover))) {
        ++stats_.snapshot_rejected; return true;
    }
    const sse_live::Snapshot previous = state.previous_valid ? state.previous : value;
    state.previous = value; state.previous_valid = true;
    if (value.time_of_day_micros - previous.time_of_day_micros > 4000000ULL) {
        ++stats_.snapshot_rejected; return true;
    }
    const std::vector<float> factors = sse_snapshot36::build(previous, value);
    std::vector<float> enhanced(factors);
    const auto auction = auction_.find(symbol);
    if (auction == auction_.end()) { enhanced.resize(95, std::numeric_limits<float>::quiet_NaN()); ++stats_.missing_auction; }
    else enhanced.insert(enhanced.end(), auction->second.begin(), auction->second.end());
    sse_snapshot_gru::Prediction prediction;
    const auto started = now_ns();
    const bool ok = snapshot_.predict(factors, enhanced, "sse", value.time_of_day_micros, &state.model, &prediction, &error_);
    stats_.snapshot_infer.observe(now_ns() - started);
    if (!ok || !prediction.valid) { ++stats_.model_errors; return false; }
    ++stats_.snapshot_predictions; ++stock_predictions_[symbol];
    stats_.prediction_sum += prediction.ensemble_pred;
    return true;
}
}
