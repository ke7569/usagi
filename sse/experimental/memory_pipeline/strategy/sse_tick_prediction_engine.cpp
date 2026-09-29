#include "sse_tick_prediction_engine.h"
#include "sse_tick_sample_gate.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sse_tick_strategy {

namespace {

std::uint64_t stage_clock_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

PredictionRow::PredictionRow()
    : channel_no(0), tick_index(0), exchange_time_micros(0), factor_ns(0),
      factor_l1_ns(0), factor_flow_ns(0), factor_depth_build_ns(0),
      factor_depth_aggregate_ns(0), factor_finalize_ns(0), flow_event_count(0),
      live_order_count(0), bid_level_count(0), ask_level_count(0), infer_ns(0), factors(), prediction(0.0f),
      model_valid(false), factor_complete(false), selected(false), quality("uninitialized") {}

PredictionEngine::PredictionEngine() : states_(), model_(), loaded_(false) {}
PredictionEngine::~PredictionEngine() { reset(); }

bool PredictionEngine::load(const std::string& model_path, std::string* error) {
    loaded_ = model_.load(model_path, error);
    return loaded_;
}

bool PredictionEngine::set_free_share(const std::string& security_id,
                                      double free_share) {
    if (security_id.empty() || free_share <= 0.0 || !std::isfinite(free_share))
        return false;
    std::unordered_map<std::string, SecurityState*>::iterator it = states_.find(security_id);
    if (it == states_.end()) {
        states_[security_id] = new SecurityState(security_id);
        it = states_.find(security_id);
    }
    it->second->factors.set_free_share(free_share);
    return true;
}

bool PredictionEngine::set_static_metadata(
    const std::string& security_id,
    const sse_tick::DailyStaticMetadata& metadata) {
    if (!sse_live::is_sse_stock(security_id)) return false;
    std::unordered_map<std::string, SecurityState*>::iterator it = states_.find(security_id);
    if (it == states_.end()) {
        states_[security_id] = new SecurityState(security_id);
        it = states_.find(security_id);
    }
    it->second->factors.set_static_metadata(metadata);
    // CompleteOrderBookSH starts its first continuous window at the 09:25
    // pre-close cut. Seed that fallback before pre-open ticks are replayed so
    // the first live sample includes auction flow but has the reference mid.
    if (metadata.has_pre_close && metadata.pre_close > 0.0)
        it->second->factors.seed_window(metadata.pre_close, 33900000000ULL);
    it->second->have_sample_reference = metadata.has_pre_close && metadata.pre_close > 0.0;
    it->second->last_sample_turnover = 0.0;
    it->second->last_sample_mid = metadata.has_pre_close ? metadata.pre_close : 0.0;
    it->second->last_sample_volume = 0U;
    return true;
}

void PredictionEngine::reset() {
    for (std::unordered_map<std::string, SecurityState*>::iterator it = states_.begin();
         it != states_.end(); ++it) delete it->second;
    states_.clear();
}

bool PredictionEngine::observe(const sse_live::TickEvent& event,
                               std::uint64_t local_timestamp_micros,
                               std::uint64_t next_local_timestamp_micros,
                               PredictionRow* output, std::string* error) {
    (void)local_timestamp_micros;
    (void)next_local_timestamp_micros;
    if (error) error->clear();
    if (!output || !loaded_) {
        if (error) *error = "tick prediction engine is not loaded";
        return false;
    }
    *output = PredictionRow();
    std::unordered_map<std::string, SecurityState*>::iterator it = states_.find(event.security_id);
    if (it == states_.end()) {
        SecurityState* state = new SecurityState(event.security_id);
        states_[event.security_id] = state;
        it = states_.find(event.security_id);
    }
    SecurityState& state = *it->second;
    const sse_tick::ApplyResult applied = state.book.apply(event);
    if (!applied.accepted || !applied.sequence_healthy) {
        if (error) *error = applied.reason;
        output->quality = applied.reason;
        return false;
    }
    output->quality = "state_updated_no_sample";
    return true;
}

bool PredictionEngine::observe_snapshot(const sse_live::Snapshot& snapshot,
                                        std::string* error) {
    if (error) error->clear();
    if (!sse_live::is_sse_stock(snapshot.security_id)) {
        if (error) *error = "invalid snapshot security";
        return false;
    }
    if (!std::isfinite(snapshot.last_price) || !std::isfinite(snapshot.turnover) ||
        snapshot.last_price < 0.0 || snapshot.turnover < 0.0 || snapshot.volume < 0) {
        if (error) *error = "invalid snapshot cumulative fields";
        return false;
    }
    std::unordered_map<std::string, SecurityState*>::iterator it = states_.find(snapshot.security_id);
    if (it == states_.end()) {
        states_[snapshot.security_id] = new SecurityState(snapshot.security_id);
        it = states_.find(snapshot.security_id);
    }
    SecurityState& state = *it->second;
    // The merged input is ordered by NIC arrival.  Ignore an older snapshot
    // if a source/replay file contains a late record, preserving monotonic
    // cumulative values for the factor window.
    if (state.have_snapshot_aux && snapshot.time_of_day_micros < state.snapshot_time_micros)
        return true;
    state.have_snapshot_aux = true;
    state.snapshot_last_price = snapshot.last_price;
    state.snapshot_volume = static_cast<double>(snapshot.volume);
    state.snapshot_turnover = snapshot.turnover;
    state.snapshot_time_micros = snapshot.time_of_day_micros;
    return true;
}

bool PredictionEngine::sample_tick(const sse_live::TickEvent& event,
                                   std::uint64_t local_timestamp_micros,
                                   std::uint64_t next_local_timestamp_micros,
                                   PredictionRow* output, std::string* error) {
    if (error) error->clear();
    if (!output || !loaded_) {
        if (error) *error = "tick prediction engine is not loaded";
        return false;
    }
    *output = PredictionRow();
    if (next_local_timestamp_micros <= local_timestamp_micros ||
        next_local_timestamp_micros - local_timestamp_micros <= 100U) {
        output->quality = "not_batch_end";
        return true;
    }
    std::unordered_map<std::string, SecurityState*>::iterator it = states_.find(event.security_id);
    if (it == states_.end()) {
        output->quality = "no_order_book_state";
        return false;
    }
    SecurityState& state = *it->second;
    if (state.book.last_tick_index() == 0U) {
        output->quality = "no_order_book_state";
        return false;
    }

    sse_tick::Level bids[1] = {}, asks[1] = {};
    state.book.snapshot(bids, asks, 1U);
    const bool two_sided = bids[0].quantity > 0U && asks[0].quantity > 0U &&
                           bids[0].price_raw > 0 && asks[0].price_raw > 0;
    // CompleteOrderBookSH uses the continuous T stream as its cumulative
    // source.  A Level2 snapshot is a fallback for a security with no usable
    // T record yet; using its coarser 3-second staircase in the normal path
    // would make the replay diverge from the reference sample.  Snapshot
    // volume is already in model share units, while the factor API accepts
    // raw EFH units, hence the *1000 conversion in the fallback.
    const bool have_tick_volume = state.book.total_trade_qty() != 0U;
    const bool have_tick_turnover = state.book.total_trade_turnover() > 0.0;
    const bool have_tick_last = state.book.last_trade_price() > 0.0;
    const double current_last_price = have_tick_last ? state.book.last_trade_price()
        : (state.have_snapshot_aux ? state.snapshot_last_price : 0.0);
    const double current_volume = have_tick_volume
        ? static_cast<double>(state.book.total_trade_qty())
        : (state.have_snapshot_aux ? state.snapshot_volume : 0.0);
    const double current_turnover = have_tick_turnover
        ? state.book.total_trade_turnover()
        : (state.have_snapshot_aux ? state.snapshot_turnover : 0.0);
    const double current_mid = two_sided
        ? (static_cast<double>(bids[0].price_raw) + static_cast<double>(asks[0].price_raw)) / 2000.0
        : (current_last_price > 0.0 ? current_last_price
                                               : state.last_sample_mid);
    // Batch flow window (canonical shares/yuan) since the last sample or
    // since the last gate evaluation.  Reading the window does not consume
    // it; factor build consumes it only when the batch actually samples.
    const double batch_turnover = state.book.flow().trade_turnover;
    const std::uint64_t batch_trade_shares = state.book.flow().trade_shares;
    const bool mid_changed = state.last_sample_mid <= 0.0 ||
                             std::fabs(current_mid - state.last_sample_mid) > 1e-6;
    SampleGateInput gate_input;
    gate_input.batch_trade_shares = batch_trade_shares;
    gate_input.batch_turnover_yuan = batch_turnover;
    gate_input.amount_threshold_yuan = state.factors.turnover_threshold();
    gate_input.quiet_batch_end = true;
    gate_input.mid_changed = mid_changed;
    const char* gate_reason = 0;
    const SampleGateResult gate = evaluate_sample_gate(gate_input, &gate_reason);
    if (gate != kSampleGateSample) {
        // The batch did not qualify (e.g. trades below 100 shares, or no
        // amount/mid trigger).  Discard the batch flow window so the next
        // gate evaluation measures only the next batch; never emit a sample.
        (void)state.book.take_flow_window();
        output->quality = gate_reason == 0 ? "sample_condition_not_met" : gate_reason;
        return true;
    }

    const std::uint64_t factor_started = stage_clock_ns();
    const sse_tick::FactorRow factors = state.factors.build(
        state.book, event.time_of_day_micros, current_last_price,
        current_volume, current_turnover);
    output->factor_ns = stage_clock_ns() - factor_started;
    output->factor_l1_ns = factors.factor_l1_ns;
    output->factor_flow_ns = factors.factor_flow_ns;
    output->factor_depth_build_ns = factors.factor_depth_build_ns;
    output->factor_depth_aggregate_ns = factors.factor_depth_aggregate_ns;
    output->factor_finalize_ns = factors.factor_finalize_ns;
    output->factors = factors.values;
    output->flow_event_count = factors.flow_event_count;
    output->live_order_count = factors.live_order_count;
    output->bid_level_count = factors.bid_level_count;
    output->ask_level_count = factors.ask_level_count;
    std::array<float, sse_model::kFeatureCount> input = factors.values;
    float prediction = 0.0f;
    const std::uint64_t infer_started = stage_clock_ns();
    const bool model_valid = model_.predict(input, &state.model_state, &prediction);
    output->infer_ns = stage_clock_ns() - infer_started;
    if (!model_valid) {
        if (error) *error = "SSEMODL1 prediction failed";
        output->quality = "model_failed";
        return false;
    }
    output->security_id = event.security_id;
    output->channel_no = event.channel_no;
    output->tick_index = state.book.last_tick_index();
    output->exchange_time_micros = event.time_of_day_micros;
    output->prediction = prediction;
    output->model_valid = true;
    output->factor_complete = factors.validity.complete;
    output->selected = factors.validity.complete;
    if (factors.validity.complete) output->quality = "ok";
    else if (!factors.validity.has_static_metadata) output->quality = "static_metadata_missing";
    else if (!factors.validity.static_metadata_complete) output->quality = "static_metadata_incomplete";
    else if (!factors.validity.has_free_share) output->quality = "static_free_share_missing";
    else output->quality = "factor_incomplete_shadow";
    state.have_sample_reference = true;
    state.last_sample_turnover = current_turnover;
    state.last_sample_mid = factors.mid_price > 0.0 ? factors.mid_price : current_mid;
    state.last_sample_volume = static_cast<std::uint64_t>(current_volume);
    return true;
}

bool PredictionEngine::sample_level2(const std::string& security_id,
                                     std::uint32_t channel_no,
                                     std::uint64_t exchange_time_micros,
                                     std::uint64_t local_timestamp_micros,
                                     std::uint64_t next_local_timestamp_micros,
                                     const sse_live::Snapshot* snapshot,
                                     PredictionRow* output, std::string* error) {
    if (error) error->clear();
    if (!output || !loaded_) {
        if (error) *error = "tick prediction engine is not loaded";
        return false;
    }
    *output = PredictionRow();
    if (next_local_timestamp_micros <= local_timestamp_micros ||
        next_local_timestamp_micros - local_timestamp_micros <= 100U) {
        output->quality = "not_batch_end";
        return true;
    }
    std::unordered_map<std::string, SecurityState*>::iterator it = states_.find(security_id);
    if (it == states_.end()) {
        output->quality = "no_order_book_state";
        return false;
    }
    SecurityState& state = *it->second;
    if (state.book.last_tick_index() == 0U) {
        output->quality = "no_order_book_state";
        return false;
    }
    sse_tick::Level bids[1] = {}, asks[1] = {};
    state.book.snapshot(bids, asks, 1U);
    const bool two_sided = bids[0].quantity > 0U && asks[0].quantity > 0U &&
                           bids[0].price_raw > 0 && asks[0].price_raw > 0;
    const double current_mid = two_sided
        ? (static_cast<double>(bids[0].price_raw) + static_cast<double>(asks[0].price_raw)) / 2000.0
        : (snapshot && snapshot->last_price > 0.0 ? snapshot->last_price : state.last_sample_mid);
    // Sampling windows are driven by the ordered tick stream.  The snapshot
    // values remain the factor fallback, but may lag the final ticks in a
    // batch and therefore must not be used for the gate when tick totals are
    // available.
    const double current_volume = state.book.total_trade_qty() != 0U
        ? static_cast<double>(state.book.total_trade_qty())
        : (snapshot ? static_cast<double>(snapshot->volume) : 0.0);
    const double current_turnover = state.book.total_trade_turnover() > 0.0
        ? state.book.total_trade_turnover()
        : (snapshot ? snapshot->turnover : 0.0);
    const double turnover_delta = current_turnover - state.last_sample_turnover;
    const double volume_delta = current_volume - static_cast<double>(state.last_sample_volume);
    const bool mid_changed = state.last_sample_mid <= 0.0 ||
                             std::fabs(current_mid - state.last_sample_mid) > 1e-6;
    const double batch_turnover = state.book.flow().trade_turnover;
    const std::uint64_t batch_trade_shares = state.book.flow().trade_shares;
    SampleGateInput gate_input;
    gate_input.batch_trade_shares = batch_trade_shares;
    gate_input.batch_turnover_yuan = batch_turnover > 0.0
        ? batch_turnover : turnover_delta;
    gate_input.amount_threshold_yuan = state.factors.turnover_threshold();
    gate_input.quiet_batch_end = true;
    gate_input.mid_changed = mid_changed;
    const char* gate_reason = 0;
    const SampleGateResult gate = evaluate_sample_gate(gate_input, &gate_reason);
    if (gate != kSampleGateSample) {
        (void)state.book.take_flow_window();
        (void)volume_delta;
        output->quality = gate_reason == 0 ? "sample_condition_not_met" : gate_reason;
        return true;
    }
    const std::uint64_t factor_started = stage_clock_ns();
    const sse_tick::FactorRow factors = snapshot == 0
        ? state.factors.build(state.book, exchange_time_micros)
        : state.factors.build(state.book, exchange_time_micros,
                              snapshot->last_price,
                              static_cast<double>(snapshot->volume),
                              snapshot->turnover);
    output->factor_ns = stage_clock_ns() - factor_started;
    output->factor_l1_ns = factors.factor_l1_ns;
    output->factor_flow_ns = factors.factor_flow_ns;
    output->factor_depth_build_ns = factors.factor_depth_build_ns;
    output->factor_depth_aggregate_ns = factors.factor_depth_aggregate_ns;
    output->factor_finalize_ns = factors.factor_finalize_ns;
    output->factors = factors.values;
    output->flow_event_count = factors.flow_event_count;
    output->live_order_count = factors.live_order_count;
    output->bid_level_count = factors.bid_level_count;
    output->ask_level_count = factors.ask_level_count;
    std::array<float, sse_model::kFeatureCount> input = factors.values;
    float prediction = 0.0f;
    const std::uint64_t infer_started = stage_clock_ns();
    const bool model_valid = model_.predict(input, &state.model_state, &prediction);
    output->infer_ns = stage_clock_ns() - infer_started;
    if (!model_valid) {
        if (error) *error = "SSEMODL1 prediction failed";
        output->quality = "model_failed";
        return false;
    }
    output->security_id = security_id;
    output->channel_no = channel_no;
    output->tick_index = state.book.last_tick_index();
    output->exchange_time_micros = exchange_time_micros;
    output->prediction = prediction;
    output->model_valid = true;
    output->factor_complete = factors.validity.complete;
    output->selected = factors.validity.complete;
    if (factors.validity.complete) output->quality = "ok";
    else if (!factors.validity.has_static_metadata) output->quality = "static_metadata_missing";
    else if (!factors.validity.static_metadata_complete) output->quality = "static_metadata_incomplete";
    else if (!factors.validity.has_free_share) output->quality = "static_free_share_missing";
    else output->quality = "factor_incomplete_shadow";
    state.have_sample_reference = true;
    state.last_sample_turnover = current_turnover;
    state.last_sample_mid = factors.mid_price > 0.0 ? factors.mid_price : current_mid;
    state.last_sample_volume = static_cast<std::uint64_t>(current_volume);
    return true;
}

}  // namespace sse_tick_strategy
