#include "sze/market_data/sz_hp_realtime_state.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace sz_hp {

namespace {

inline bool almost_equal(double left, double right, double tolerance) {
    return std::fabs(left - right) < tolerance;
}

}  // namespace

SamplerConfig::SamplerConfig()
    : history_amount_threshold(std::numeric_limits<double>::max()),
      downsample(kDefaultDownsample),
      turnover_tolerance(kTurnoverTolerance),
      minimum_volume_delta(kMinimumSampleVolume),
      fee_share(1.0),
      price_tick(0.01),
      upper_price(std::numeric_limits<double>::max()),
      lower_price(0.0),
      capture_failure_digest(false) {
}

SampleDecision::SampleDecision()
    : ready(false),
      book_available(true),
      turnover_reconciled(false),
      mid_changed(false),
      downsample_reached(false),
      first_afternoon(false),
      changed_millisecond(false),
      volume_ready(false),
      trigger(SampleTrigger::kNone),
      reason(SampleBlockReason::kNone),
      sequence(0),
      event_time_ms(0),
      turnover_delta(0.0),
      cumulative_amount(0.0) {
}

OrderFlowSample::OrderFlowSample()
    : order_pf(0.0),
      order_nf(0.0),
      order_mf(0.0),
      trade_pcf(0.0),
      trade_ncf(0.0),
      trade_pt(0.0),
      trade_nt(0.0),
      raw_trade_pcf(0.0),
      raw_trade_ncf(0.0),
      raw_trade_pt(0.0),
      raw_trade_nt(0.0),
      cxl_buy_flow(0.0),
      cxl_sell_flow(0.0),
      buy_order_volume(0.0),
      sell_order_volume(0.0) {
}

SampleBatch::SampleBatch()
    : order_flow(),
      previous_observation(),
      current_observation(),
      decision(),
      sample_index(0),
      application_sequence(0),
      cumulative_amount(0.0),
      valid(false) {
}

InstrumentState::InstrumentState(const std::string& instrument,
                                 const SamplerConfig& config)
    : instrument_(instrument),
      config_(config),
      book_(instrument),
      order_queue_(),
      trade_queue_(),
      previous_observation_(),
      current_observation_(),
      has_previous_observation_(false),
      has_current_observation_(false),
      market_data_valid_(true),
      sample_pending_(false),
      last_application_sequence_(0),
      sample_count_(0),
      cumulative_fill_amount_(0.0),
      last_volume_(0.0),
      last_turnover_(0.0),
      pending_decision_(),
      pre_event_digest_(),
      pre_failure_digest_() {
    order_queue_.reserve(512);
    trade_queue_.reserve(512);
    if (!(config_.fee_share > 0.0)) {
        config_.fee_share = 1.0;
    }
}

// Semantic port of PredictionDSChange::HandleOD plus MatchBook<SZ>::UpdateMD.
bool InstrumentState::process_order(const OrderEvent& event) {
    if (!book_.available()) {
        // MatchBook freezes its levels after a failure, while PredictionDSChange
        // still records subsequent order events in its cache and sequence state.
        order_queue_.push_back(event);
        last_application_sequence_ = event.sequence;
        return true;
    }
    capture_pre_event_digest();
    order_queue_.push_back(event);
    last_application_sequence_ = event.sequence;
    const bool success = book_.update_order(event);
    if (!success) {
        sample_pending_ = false;
        record_failure_digest();
    }
    return success;
}

// Semantic port of PredictionDSChange::HandleTD. It never extracts factors or runs a model.
SampleDecision InstrumentState::process_trade(const TradeEvent& event) {
    SampleDecision decision;
    decision.trigger = SampleTrigger::kTrade;
    decision.sequence = event.sequence;
    decision.event_time_ms = event.event_time_ms;
    decision.book_available = book_.available();
    if (!book_.available()) {
        if (!market_data_valid_) {
            cumulative_fill_amount_ = 0.0;
            decision.reason = SampleBlockReason::kMarketDataInvalid;
            return decision;
        }
        trade_queue_.push_back(event);
        if (event.raw_trade_flag != '4') {
            cumulative_fill_amount_ += event_price(event) *
                                       static_cast<double>(event.quantity);
        } else {
            last_application_sequence_ = event.sequence;
        }
        decision.reason = SampleBlockReason::kUnavailable;
        return decision;
    }

    capture_pre_event_digest();
    if (!market_data_valid_) {
        cumulative_fill_amount_ = 0.0;
        if (!book_.update_trade(event)) {
            decision.book_available = false;
            decision.reason = SampleBlockReason::kUnavailable;
            record_failure_digest();
        } else {
            decision.reason = SampleBlockReason::kMarketDataInvalid;
        }
        return decision;
    }

    trade_queue_.push_back(event);
    if (event.raw_trade_flag != '4') {
        cumulative_fill_amount_ += event_price(event) * static_cast<double>(event.quantity);
    } else {
        last_application_sequence_ = event.sequence;
    }

    bool turnover_reconciled = false;
    bool mid_changed = false;
    bool downsample_reached = false;
    bool first_afternoon = false;
    double turnover_delta = 0.0;
    if (event.raw_trade_flag != '4' && has_previous_observation_ && has_current_observation_) {
        turnover_delta = current_observation_.turnover - last_turnover_;
        turnover_reconciled = cumulative_fill_amount_ >=
                              turnover_delta - config_.turnover_tolerance;
        mid_changed = !almost_equal(current_observation_.fast_mid_price(),
                                    previous_observation_.fast_mid_price(), 1e-6);
        downsample_reached = cumulative_fill_amount_ * static_cast<double>(config_.downsample) >
                             config_.history_amount_threshold;
        first_afternoon = is_first_afternoon(event.event_time_ms);
    }

    const bool success = book_.update_trade(event);
    decision.book_available = book_.available();
    if (!success) {
        sample_pending_ = false;
        decision.reason = SampleBlockReason::kUnavailable;
        record_failure_digest();
        return decision;
    }
    if (event.raw_trade_flag == '4' || !has_previous_observation_ || !has_current_observation_) {
        decision.reason = SampleBlockReason::kNoCurrentObservation;
        return decision;
    }
    if (!turnover_reconciled) {
        decision.turnover_delta = turnover_delta;
        decision.cumulative_amount = cumulative_fill_amount_;
        decision.reason = SampleBlockReason::kTurnoverIncomplete;
        return decision;
    }
    if (!downsample_reached && !mid_changed && !first_afternoon) {
        decision.turnover_delta = turnover_delta;
        decision.cumulative_amount = cumulative_fill_amount_;
        decision.reason = SampleBlockReason::kTriggerNotMet;
        return decision;
    }
    return evaluate_candidate(SampleTrigger::kTrade,
                              current_observation_.event_time_ms,
                              turnover_reconciled,
                              mid_changed,
                              downsample_reached,
                              first_afternoon,
                              turnover_delta);
}

// Semantic port of PredictionDSChange::HandleOB and MayPrediction.
SampleDecision InstrumentState::process_observation(const MarketObservation& observation) {
    SampleDecision decision;
    decision.trigger = SampleTrigger::kObservation;
    decision.event_time_ms = observation.event_time_ms;
    decision.sequence = last_application_sequence_;
    decision.book_available = book_.available();
    if (!book_.available()) {
        decision.reason = SampleBlockReason::kUnavailable;
        return decision;
    }
    if (!observation.valid) {
        has_previous_observation_ = false;
        has_current_observation_ = false;
        previous_observation_ = MarketObservation();
        current_observation_ = MarketObservation();
        last_volume_ = 0.0;
        market_data_valid_ = false;
        sample_pending_ = false;
        decision.reason = SampleBlockReason::kInvalidObservation;
        return decision;
    }
    if (!has_previous_observation_) {
        previous_observation_ = observation;
        has_previous_observation_ = true;
        decision.reason = SampleBlockReason::kFirstObservation;
        return decision;
    }

    market_data_valid_ = true;
    if (previous_observation_.event_time_ms < kMarketOpenTimeMs) {
        last_volume_ = 0.0;
        last_turnover_ = 0.0;
    } else {
        last_volume_ = previous_observation_.total_volume;
        last_turnover_ = previous_observation_.turnover;
    }
    current_observation_ = observation;
    has_current_observation_ = true;

    const double turnover_delta = current_observation_.turnover - last_turnover_;
    const bool turnover_reconciled = is_turnover_reconciled(turnover_delta);
    const bool mid_changed = !almost_equal(current_observation_.fast_mid_price(),
                                           previous_observation_.fast_mid_price(), 1e-6);
    const bool downsample_reached = cumulative_fill_amount_ *
                                        static_cast<double>(config_.downsample) >
                                    config_.history_amount_threshold;
    const bool first_afternoon = is_first_afternoon(observation.event_time_ms);

    bool candidate = false;
    SampleBlockReason blocked = SampleBlockReason::kTriggerNotMet;
    if (first_afternoon) {
        candidate = turnover_reconciled;
        blocked = SampleBlockReason::kTurnoverIncomplete;
    } else if (!mid_changed) {
        candidate = turnover_reconciled && downsample_reached;
        blocked = turnover_reconciled ? SampleBlockReason::kTriggerNotMet
                                      : SampleBlockReason::kTurnoverIncomplete;
    } else if (turnover_delta > cumulative_fill_amount_ + config_.turnover_tolerance) {
        blocked = SampleBlockReason::kTurnoverIncomplete;
    } else {
        candidate = true;
    }

    if (!candidate) {
        decision.turnover_reconciled = turnover_reconciled;
        decision.mid_changed = mid_changed;
        decision.downsample_reached = downsample_reached;
        decision.first_afternoon = first_afternoon;
        decision.turnover_delta = turnover_delta;
        decision.cumulative_amount = cumulative_fill_amount_;
        decision.reason = blocked;
        return decision;
    }
    return evaluate_candidate(SampleTrigger::kObservation,
                              observation.event_time_ms,
                              turnover_reconciled,
                              mid_changed,
                              downsample_reached,
                              first_afternoon,
                              turnover_delta);
}

void InstrumentState::reject_event(uint64_t sequence, const char* reason) {
    if (!book_.available()) {
        return;
    }
    capture_pre_event_digest();
    sample_pending_ = false;
    book_.reject(sequence, reason);
    record_failure_digest();
}

bool InstrumentState::consume_sample(SampleBatch* destination) {
    if (destination == 0 || !sample_pending_ || !book_.available() ||
        !has_previous_observation_ || !has_current_observation_) {
        return false;
    }

    SampleBatch batch;
    batch.previous_observation = previous_observation_;
    batch.current_observation = current_observation_;
    batch.decision = pending_decision_;
    batch.sample_index = sample_count_ + 1;
    batch.application_sequence = last_application_sequence_;
    batch.cumulative_amount = cumulative_fill_amount_;

    // PredictionDSChange::Get28FFactor consumes every cached order before every trade.
    for (size_t i = 0; i < order_queue_.size(); ++i) {
        consume_order(order_queue_[i], &batch.order_flow);
    }
    for (size_t i = 0; i < trade_queue_.size(); ++i) {
        consume_trade(trade_queue_[i], &batch.order_flow);
    }
    order_queue_.clear();
    trade_queue_.clear();

    const double inverse_fee_share = 1.0 / config_.fee_share;
    batch.order_flow.order_mf *= inverse_fee_share;
    batch.order_flow.trade_pcf = batch.order_flow.raw_trade_pcf * inverse_fee_share;
    batch.order_flow.trade_ncf = batch.order_flow.raw_trade_ncf * inverse_fee_share;
    batch.order_flow.trade_pt = batch.order_flow.raw_trade_pt * inverse_fee_share;
    batch.order_flow.trade_nt = batch.order_flow.raw_trade_nt * inverse_fee_share;
    batch.valid = true;

    previous_observation_ = current_observation_;
    has_previous_observation_ = true;
    cumulative_fill_amount_ = 0.0;
    sample_pending_ = false;
    pending_decision_ = SampleDecision();
    ++sample_count_;
    *destination = batch;
    return true;
}

const std::string& InstrumentState::instrument() const {
    return instrument_;
}

const OrderBook& InstrumentState::book() const {
    return book_;
}

OrderBook& InstrumentState::book() {
    return book_;
}

const SamplerConfig& InstrumentState::config() const {
    return config_;
}

bool InstrumentState::available() const {
    return book_.available();
}

bool InstrumentState::market_data_valid() const {
    return market_data_valid_;
}

bool InstrumentState::has_previous_observation() const {
    return has_previous_observation_;
}

bool InstrumentState::has_current_observation() const {
    return has_current_observation_;
}

bool InstrumentState::sample_pending() const {
    return sample_pending_;
}

uint64_t InstrumentState::last_application_sequence() const {
    return last_application_sequence_;
}

uint64_t InstrumentState::sample_count() const {
    return sample_count_;
}

double InstrumentState::cumulative_fill_amount() const {
    return cumulative_fill_amount_;
}

size_t InstrumentState::queued_order_count() const {
    return order_queue_.size();
}

size_t InstrumentState::queued_trade_count() const {
    return trade_queue_.size();
}

const MarketObservation* InstrumentState::previous_observation() const {
    return has_previous_observation_ ? &previous_observation_ : 0;
}

const MarketObservation* InstrumentState::current_observation() const {
    return has_current_observation_ ? &current_observation_ : 0;
}

const std::string& InstrumentState::pre_failure_digest() const {
    return pre_failure_digest_;
}

std::string InstrumentState::digest() const {
    std::ostringstream out;
    out << book_.digest()
        << ";md_valid=" << (market_data_valid_ ? 1 : 0)
        << ";has_prev=" << (has_previous_observation_ ? 1 : 0)
        << ";has_cur=" << (has_current_observation_ ? 1 : 0)
        << ";last_seq=" << last_application_sequence_
        << ";queued=" << order_queue_.size() << "," << trade_queue_.size()
        << ";cum_amount=" << std::setprecision(17) << cumulative_fill_amount_
        << ";sample_pending=" << (sample_pending_ ? 1 : 0)
        << ";sample_count=" << sample_count_;
    if (has_previous_observation_) {
        out << ";prev=" << previous_observation_.event_time_ms << ","
            << previous_observation_.total_volume << "," << previous_observation_.turnover;
    }
    if (has_current_observation_) {
        out << ";cur=" << current_observation_.event_time_ms << ","
            << current_observation_.total_volume << "," << current_observation_.turnover;
    }
    return out.str();
}

void InstrumentState::reset() {
    book_.clear();
    order_queue_.clear();
    trade_queue_.clear();
    previous_observation_ = MarketObservation();
    current_observation_ = MarketObservation();
    has_previous_observation_ = false;
    has_current_observation_ = false;
    market_data_valid_ = true;
    sample_pending_ = false;
    last_application_sequence_ = 0;
    sample_count_ = 0;
    cumulative_fill_amount_ = 0.0;
    last_volume_ = 0.0;
    last_turnover_ = 0.0;
    pending_decision_ = SampleDecision();
    pre_event_digest_.clear();
    pre_failure_digest_.clear();
}

SampleDecision InstrumentState::evaluate_candidate(SampleTrigger trigger,
                                                   uint32_t trigger_time_ms,
                                                   bool turnover_reconciled,
                                                   bool mid_changed,
                                                   bool downsample_reached,
                                                   bool first_afternoon,
                                                   double turnover_delta) {
    SampleDecision decision;
    decision.trigger = trigger;
    decision.sequence = last_application_sequence_;
    decision.event_time_ms = trigger_time_ms;
    decision.book_available = book_.available();
    decision.turnover_reconciled = turnover_reconciled;
    decision.mid_changed = mid_changed;
    decision.downsample_reached = downsample_reached;
    decision.first_afternoon = first_afternoon;
    decision.turnover_delta = turnover_delta;
    decision.cumulative_amount = cumulative_fill_amount_;

    if (sample_pending_) {
        decision.reason = SampleBlockReason::kAlreadyPending;
        return decision;
    }
    if (!book_.available()) {
        decision.reason = SampleBlockReason::kUnavailable;
        return decision;
    }
    if (!market_data_valid_) {
        decision.reason = SampleBlockReason::kMarketDataInvalid;
        return decision;
    }
    if (!has_previous_observation_ || !has_current_observation_) {
        decision.reason = SampleBlockReason::kNoCurrentObservation;
        return decision;
    }
    if (trigger_time_ms < kMarketOpenTimeMs) {
        decision.reason = SampleBlockReason::kBeforeOpen;
        return decision;
    }
    if (current_observation_.ask_volume[0] == 0.0) {
        decision.reason = SampleBlockReason::kInsufficientAsk;
        return decision;
    }
    decision.volume_ready = current_observation_.total_volume - last_volume_ >=
                            config_.minimum_volume_delta;
    if (!decision.volume_ready) {
        decision.reason = SampleBlockReason::kInsufficientVolume;
        return decision;
    }
    decision.changed_millisecond = previous_observation_.event_time_ms !=
                                   current_observation_.event_time_ms;
    if (!decision.changed_millisecond) {
        decision.reason = SampleBlockReason::kSameMillisecond;
        return decision;
    }

    decision.ready = true;
    decision.reason = SampleBlockReason::kNone;
    sample_pending_ = true;
    pending_decision_ = decision;
    return decision;
}

bool InstrumentState::is_first_afternoon(uint32_t event_time_ms) const {
    if (!has_previous_observation_ || event_time_ms < kAfternoonOpenTimeMs) {
        return false;
    }
    return event_time_ms > previous_observation_.event_time_ms &&
           event_time_ms - previous_observation_.event_time_ms > kAfternoonGapMs;
}

bool InstrumentState::is_turnover_reconciled(double turnover_delta) const {
    return std::fabs(turnover_delta - cumulative_fill_amount_) < config_.turnover_tolerance;
}

void InstrumentState::consume_order(const OrderEvent& event, OrderFlowSample* flow) const {
    if (flow == 0 || !has_previous_observation_) {
        return;
    }
    const double price = event_price(event);
    const double quantity = static_cast<double>(event.quantity);
    const double bid = previous_observation_.bid_price[0];
    const double ask = previous_observation_.ask_price[0];
    const double bid_limit = previous_observation_.ask_volume[0] == 0.0
                                 ? bid + config_.price_tick
                                 : ask;
    const double ask_limit = previous_observation_.bid_volume[0] == 0.0
                                 ? ask - config_.price_tick
                                 : bid;

    if (event.is_buy) {
        if (price > 0.0 && bid > 0.0 && price < bid_limit - 1e-6) {
            const double weight = 1.0 - std::tanh((bid / price - 1.0) * 100.0);
            flow->order_pf += quantity / config_.fee_share * weight;
        }
        if (event.type != OrderType::kLimitPrice || price > ask + 1e-6) {
            flow->order_mf += quantity;
        }
        flow->buy_order_volume += quantity;
    } else {
        if (price > 0.0 && ask > 0.0 && price > ask_limit + 1e-6) {
            const double weight = 1.0 - std::tanh((price / ask - 1.0) * 100.0);
            flow->order_nf += quantity / config_.fee_share * weight;
        }
        if (event.type != OrderType::kLimitPrice || price < bid - 1e-6) {
            flow->order_mf -= quantity;
        }
        flow->sell_order_volume += quantity;
    }
}

void InstrumentState::consume_trade(const TradeEvent& event, OrderFlowSample* flow) const {
    if (flow == 0) {
        return;
    }
    const double quantity = static_cast<double>(event.quantity);
    if (event.raw_trade_flag == '4') {
        if (event.ask_id == 0) {
            flow->raw_trade_pcf += quantity;
            flow->cxl_buy_flow += quantity;
        }
        if (event.bid_id == 0) {
            flow->raw_trade_ncf += quantity;
            flow->cxl_sell_flow += quantity;
        }
        return;
    }
    if (event.bid_id > event.ask_id) {
        flow->raw_trade_pt += quantity;
        if (event.raw_trade_flag != 'F') {
            flow->buy_order_volume += quantity;
        }
    } else if (event.bid_id < event.ask_id) {
        flow->raw_trade_nt += quantity;
        if (event.raw_trade_flag != 'F') {
            flow->sell_order_volume += quantity;
        }
    }
}

double InstrumentState::event_price(const OrderEvent& event) const {
    return event.raw_price > 0.0
               ? event.raw_price
               : static_cast<double>(event.price) / static_cast<double>(kPriceScale);
}

double InstrumentState::event_price(const TradeEvent& event) const {
    return event.raw_price > 0.0
               ? event.raw_price
               : static_cast<double>(event.price) / static_cast<double>(kPriceScale);
}

void InstrumentState::capture_pre_event_digest() {
    if (config_.capture_failure_digest) {
        pre_event_digest_ = digest();
    }
}

void InstrumentState::record_failure_digest() {
    if (config_.capture_failure_digest) {
        pre_failure_digest_ = pre_event_digest_;
    }
}

}  // namespace sz_hp
