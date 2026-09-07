#ifndef SZ_HP_REALTIME_STATE_H
#define SZ_HP_REALTIME_STATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sze/market_data/sz_hp_orderbook.h"

namespace sz_hp {

static const uint32_t kDefaultDownsample = 8000U;
static const double kTurnoverTolerance = 0.1;
static const double kMinimumSampleVolume = 100.0;
static const uint32_t kMarketOpenTimeMs = 9U * 60U * 60U * 1000U + 30U * 60U * 1000U;
static const uint32_t kAfternoonOpenTimeMs = 13U * 60U * 60U * 1000U;
static const uint32_t kAfternoonGapMs = 100000U;

struct SamplerConfig {
    double history_amount_threshold;
    uint32_t downsample;
    double turnover_tolerance;
    double minimum_volume_delta;
    double fee_share;
    double price_tick;
    double upper_price;
    double lower_price;
    bool capture_failure_digest;

    SamplerConfig();
};

enum class SampleTrigger {
    kNone,
    kObservation,
    kTrade
};

enum class SampleBlockReason {
    kNone,
    kUnavailable,
    kInvalidObservation,
    kFirstObservation,
    kMarketDataInvalid,
    kBeforeOpen,
    kNoCurrentObservation,
    kTurnoverIncomplete,
    kTriggerNotMet,
    kInsufficientAsk,
    kInsufficientVolume,
    kSameMillisecond,
    kAlreadyPending,
    kAdapterRejected
};

struct SampleDecision {
    bool ready;
    bool book_available;
    bool turnover_reconciled;
    bool mid_changed;
    bool downsample_reached;
    bool first_afternoon;
    bool changed_millisecond;
    bool volume_ready;
    SampleTrigger trigger;
    SampleBlockReason reason;
    uint64_t sequence;
    uint32_t event_time_ms;
    double turnover_delta;
    double cumulative_amount;

    SampleDecision();
};

struct OrderFlowSample {
    double order_pf;
    double order_nf;
    double order_mf;
    double trade_pcf;
    double trade_ncf;
    double trade_pt;
    double trade_nt;

    double raw_trade_pcf;
    double raw_trade_ncf;
    double raw_trade_pt;
    double raw_trade_nt;
    double cxl_buy_flow;
    double cxl_sell_flow;
    double buy_order_volume;
    double sell_order_volume;

    OrderFlowSample();
};

struct SampleBatch {
    OrderFlowSample order_flow;
    MarketObservation previous_observation;
    MarketObservation current_observation;
    SampleDecision decision;
    uint64_t sample_index;
    uint64_t application_sequence;
    double cumulative_amount;
    bool valid;

    SampleBatch();
};

class InstrumentState {
public:
    explicit InstrumentState(const std::string& instrument = std::string(),
                             const SamplerConfig& config = SamplerConfig());

    bool process_order(const OrderEvent& event);
    SampleDecision process_trade(const TradeEvent& event);
    SampleDecision process_observation(const MarketObservation& observation);
    void reject_event(uint64_t sequence, const char* reason);
    bool consume_sample(SampleBatch* destination);

    const std::string& instrument() const;
    const OrderBook& book() const;
    OrderBook& book();
    const SamplerConfig& config() const;
    bool available() const;
    bool market_data_valid() const;
    bool has_previous_observation() const;
    bool has_current_observation() const;
    bool sample_pending() const;
    uint64_t last_application_sequence() const;
    uint64_t sample_count() const;
    double cumulative_fill_amount() const;
    size_t queued_order_count() const;
    size_t queued_trade_count() const;
    const MarketObservation* previous_observation() const;
    const MarketObservation* current_observation() const;
    const std::string& pre_failure_digest() const;
    std::string digest() const;

    void reset();

private:
    SampleDecision evaluate_candidate(SampleTrigger trigger,
                                      uint32_t trigger_time_ms,
                                      bool turnover_reconciled,
                                      bool mid_changed,
                                      bool downsample_reached,
                                      bool first_afternoon,
                                      double turnover_delta);
    bool is_first_afternoon(uint32_t event_time_ms) const;
    bool is_turnover_reconciled(double turnover_delta) const;
    void consume_order(const OrderEvent& event, OrderFlowSample* flow) const;
    void consume_trade(const TradeEvent& event, OrderFlowSample* flow) const;
    double event_price(const OrderEvent& event) const;
    double event_price(const TradeEvent& event) const;
    void capture_pre_event_digest();
    void record_failure_digest();

    std::string instrument_;
    SamplerConfig config_;
    OrderBook book_;
    std::vector<OrderEvent> order_queue_;
    std::vector<TradeEvent> trade_queue_;
    MarketObservation previous_observation_;
    MarketObservation current_observation_;
    bool has_previous_observation_;
    bool has_current_observation_;
    bool market_data_valid_;
    bool sample_pending_;
    uint64_t last_application_sequence_;
    uint64_t sample_count_;
    double cumulative_fill_amount_;
    double last_volume_;
    double last_turnover_;
    SampleDecision pending_decision_;
    std::string pre_event_digest_;
    std::string pre_failure_digest_;
};

}  // namespace sz_hp

#endif  // SZ_HP_REALTIME_STATE_H
