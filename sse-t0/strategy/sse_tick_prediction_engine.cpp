#include "sse_tick_prediction_engine.h"

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
    : channel_no(0), tick_index(0), exchange_time_micros(0), factor_ns(0), infer_ns(0), prediction(0.0f),
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
    const double current_mid = two_sided
        ? (static_cast<double>(bids[0].price_raw) + static_cast<double>(asks[0].price_raw)) / 2000.0
        : (state.book.last_trade_price() > 0.0 ? state.book.last_trade_price()
                                               : state.last_sample_mid);
    const double current_turnover = state.book.total_trade_turnover();
    const double turnover_diff = current_turnover - state.last_sample_turnover;
    const double batch_turnover = state.book.flow().trade_turnover;
    const bool turnover_complete = batch_turnover + 0.1 >= turnover_diff;
    const bool mid_changed = state.last_sample_mid <= 0.0 ||
                             std::fabs(current_mid - state.last_sample_mid) > 1e-6;
    // DSChangeV4 uses the leading/infinite downsample (8000).  The metadata
    // loader already stores HistoryAmount/8000 as turnover_threshold, so the
    // batch amount is compared directly with that value.
    const double amount_threshold = state.factors.turnover_threshold();
    const bool amount_trigger = batch_turnover > amount_threshold;
    const bool first_sample = !state.have_sample_reference;
    if (!turnover_complete || (!amount_trigger && !mid_changed && !first_sample)) {
        output->quality = !turnover_complete ? "turnover_incomplete" : "sample_condition_not_met";
        return true;
    }

#ifndef SSE_DISABLE_MODEL_PREFETCH
    model_.prefetch();
#endif
    const std::uint64_t factor_started = stage_clock_ns();
    const sse_tick::FactorRow factors = state.factors.build(state.book, event.time_of_day_micros);
    output->factor_ns = stage_clock_ns() - factor_started;
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
    state.last_sample_volume = state.book.total_trade_qty();
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
#ifndef SSE_DISABLE_MODEL_PREFETCH
    model_.prefetch();
#endif
    const std::uint64_t factor_started = stage_clock_ns();
    const sse_tick::FactorRow factors = snapshot == 0
        ? state.factors.build(state.book, exchange_time_micros)
        : state.factors.build(state.book, exchange_time_micros,
                              snapshot->last_price,
                              static_cast<double>(snapshot->volume),
                              snapshot->turnover);
    output->factor_ns = stage_clock_ns() - factor_started;
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
    return true;
}

}  // namespace sse_tick_strategy
