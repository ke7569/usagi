#include "sze/market_data/sz_hp_realtime_state.h"

#include <cstring>
#include <iostream>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

sz_hp::OrderEvent make_order(uint64_t sequence,
                             uint32_t time_ms,
                             uint64_t order_id,
                             bool is_buy,
                             double price,
                             int64_t quantity) {
    sz_hp::OrderEvent event;
    std::strncpy(event.instrument.data(), "000001.SZ", event.instrument.size() - 1);
    event.sequence = sequence;
    event.order_id = order_id;
    event.event_time_ms = time_ms;
    event.price = sz_hp::to_price(price);
    event.raw_price = price;
    event.quantity = quantity;
    event.is_buy = is_buy;
    event.type = sz_hp::OrderType::kLimitPrice;
    event.raw_order_type = '2';
    return event;
}

sz_hp::TradeEvent make_trade(uint64_t sequence,
                             uint32_t time_ms,
                             uint64_t bid_id,
                             uint64_t ask_id,
                             double price,
                             int64_t quantity,
                             char flag = 'F') {
    sz_hp::TradeEvent event;
    std::strncpy(event.instrument.data(), "000001.SZ", event.instrument.size() - 1);
    event.sequence = sequence;
    event.event_time_ms = time_ms;
    event.bid_id = bid_id;
    event.ask_id = ask_id;
    event.price = sz_hp::to_price(price);
    event.raw_price = price;
    event.quantity = quantity;
    event.flag = flag == '4' ? sz_hp::TradeFlag::kCancel : sz_hp::TradeFlag::kFill;
    event.raw_trade_flag = flag;
    return event;
}

sz_hp::MarketObservation make_observation(uint32_t time_ms,
                                          double volume,
                                          double turnover,
                                          double bid_price = 10.0,
                                          double ask_price = 10.1,
                                          double bid_volume = 100.0,
                                          double ask_volume = 100.0) {
    sz_hp::MarketObservation observation;
    std::strncpy(observation.instrument.data(), "000001.SZ", observation.instrument.size() - 1);
    observation.event_time_ms = time_ms;
    observation.total_volume = volume;
    observation.turnover = turnover;
    observation.last_price = (bid_price + ask_price) / 2.0;
    observation.bid_price[0] = bid_price;
    observation.ask_price[0] = ask_price;
    observation.bid_volume[0] = bid_volume;
    observation.ask_volume[0] = ask_volume;
    observation.valid = bid_volume > 0.0 && ask_volume > 0.0;
    return observation;
}

sz_hp::SamplerConfig test_config() {
    sz_hp::SamplerConfig config;
    config.history_amount_threshold = 0.0;
    config.downsample = 1;
    config.minimum_volume_delta = 100.0;
    config.turnover_tolerance = 0.1;
    config.fee_share = 10.0;
    return config;
}

bool seed_book(sz_hp::InstrumentState* state) {
    if (!check(state != 0, "state exists")) {
        return false;
    }
    if (!check(state->book().add_order(1, true, sz_hp::to_price(10.0), 100, 34200000),
               "seed bid")) {
        return false;
    }
    if (!check(state->book().add_order(2, false, sz_hp::to_price(10.1), 100, 34200001),
               "seed ask")) {
        return false;
    }
    return true;
}

bool test_queue_and_sample() {
    sz_hp::InstrumentState state("000001.SZ", test_config());
    if (!seed_book(&state)) {
        return false;
    }
    if (!check(state.process_observation(make_observation(34200000, 1000, 1000)).reason ==
                   sz_hp::SampleBlockReason::kFirstObservation,
               "first observation initializes previous reference")) {
        return false;
    }
    if (!check(state.process_order(make_order(3, 34200001, 3, true, 10.0, 10)),
               "order is queued without sampling")) {
        return false;
    }
    const sz_hp::SampleDecision trade_decision =
        state.process_trade(make_trade(4, 34200002, 3, 2, 10.1, 10));
    if (!check(!trade_decision.ready && state.queued_order_count() == 1 &&
                   state.queued_trade_count() == 1 && state.cumulative_fill_amount() == 101.0,
               "trade mutation only queues and accumulates turnover")) {
        return false;
    }
    const sz_hp::SampleDecision same_ms =
        state.process_observation(make_observation(34200000, 1100, 1101));
    if (!check(!same_ms.ready && same_ms.reason == sz_hp::SampleBlockReason::kSameMillisecond,
               "same-millisecond observation is rejected as a sample")) {
        return false;
    }
    const sz_hp::SampleDecision ready =
        state.process_observation(make_observation(34200001, 1100, 1101));
    if (!check(ready.ready && ready.trigger == sz_hp::SampleTrigger::kObservation,
               "turnover and changed millisecond produce a sample")) {
        return false;
    }
    sz_hp::SampleBatch batch;
    if (!check(state.consume_sample(&batch) && batch.valid && batch.sample_index == 1 &&
                   batch.order_flow.buy_order_volume == 10.0 &&
                   batch.order_flow.raw_trade_pt == 10.0 &&
                   batch.order_flow.trade_pt == 1.0 && state.queued_order_count() == 0 &&
                   state.queued_trade_count() == 0 && state.cumulative_fill_amount() == 0.0,
               "sample consumes orders before trades exactly once")) {
        return false;
    }
    if (!check(!state.consume_sample(&batch) && state.sample_count() == 1,
               "sample reset prevents duplicate consumption")) {
        return false;
    }
    return true;
}

bool test_turnover_and_invalid_observation() {
    sz_hp::SamplerConfig config = test_config();
    config.history_amount_threshold = 1000000.0;
    sz_hp::InstrumentState state("000001.SZ", config);
    if (!seed_book(&state)) {
        return false;
    }
    state.process_observation(make_observation(34200000, 1000, 1000));
    state.process_trade(make_trade(3, 34200001, 1, 2, 10.0, 5));
    const sz_hp::SampleDecision incomplete =
        state.process_observation(make_observation(34200001, 1100, 1010));
    if (!check(!incomplete.ready &&
                   incomplete.reason == sz_hp::SampleBlockReason::kTurnoverIncomplete,
               "incomplete turnover blocks a mid-price sample")) {
        return false;
    }
    state.process_trade(make_trade(4, 34200002, 1, 2, 10.0, 5));
    config = test_config();
    sz_hp::InstrumentState invalid_state("000001.SZ", config);
    if (!seed_book(&invalid_state)) {
        return false;
    }
    invalid_state.process_observation(make_observation(34200000, 1000, 1000));
    invalid_state.process_order(make_order(3, 34200001, 3, true, 10.0, 10));
    invalid_state.process_trade(make_trade(4, 34200002, 3, 2, 10.0, 5));
    sz_hp::MarketObservation invalid = make_observation(34200003, 0, 0, 0, 0, 0, 0);
    if (!check(invalid_state.process_observation(invalid).reason ==
                   sz_hp::SampleBlockReason::kInvalidObservation &&
                   !invalid_state.market_data_valid() &&
                   invalid_state.queued_order_count() == 1 &&
                   invalid_state.queued_trade_count() == 1 &&
                   invalid_state.cumulative_fill_amount() == 50.0,
               "invalid observation resets references but preserves HP queues")) {
        return false;
    }
    const sz_hp::SampleDecision ignored =
        invalid_state.process_trade(make_trade(5, 34200004, 1, 2, 10.0, 5));
    if (!check(!ignored.ready && ignored.reason == sz_hp::SampleBlockReason::kMarketDataInvalid &&
                   invalid_state.queued_order_count() == 1 &&
                   invalid_state.queued_trade_count() == 1 &&
                   invalid_state.cumulative_fill_amount() == 0.0,
               "trade after invalid observation resets turnover without caching")) {
        return false;
    }
    if (!check(invalid_state.process_observation(make_observation(34200005, 1100, 1010)).reason ==
                   sz_hp::SampleBlockReason::kFirstObservation &&
                   !invalid_state.market_data_valid(),
               "first post-invalid observation only re-seeds reference")) {
        return false;
    }
    if (!check(invalid_state.process_observation(make_observation(34200006, 1200, 1010)).reason !=
                   sz_hp::SampleBlockReason::kInvalidObservation &&
                   invalid_state.market_data_valid(),
               "second post-invalid observation restores validity")) {
        return false;
    }
    return true;
}

bool test_afternoon_and_cancel_only() {
    sz_hp::SamplerConfig config = test_config();
    config.history_amount_threshold = 1000000000.0;
    sz_hp::InstrumentState afternoon("000001.SZ", config);
    if (!seed_book(&afternoon)) {
        return false;
    }
    afternoon.process_observation(make_observation(41400000, 1000, 1000));
    const sz_hp::SampleDecision afternoon_decision =
        afternoon.process_observation(make_observation(46800001, 1100, 1000));
    if (!check(afternoon_decision.ready && afternoon_decision.first_afternoon,
               "first afternoon observation can trigger a sample")) {
        return false;
    }

    sz_hp::InstrumentState cancel_only("000001.SZ", test_config());
    if (!seed_book(&cancel_only)) {
        return false;
    }
    cancel_only.process_observation(make_observation(34200000, 1000, 1000));
    const sz_hp::SampleDecision cancel =
        cancel_only.process_trade(make_trade(3, 34200001, 1, 0, 10.0, 100, '4'));
    if (!check(!cancel.ready && cancel_only.cumulative_fill_amount() == 0.0,
               "cancel-only interval does not add turnover")) {
        return false;
    }
    const sz_hp::SampleDecision changed =
        cancel_only.process_observation(make_observation(34200002, 1100, 1000, 9.9, 10.1));
    if (!check(changed.ready && changed.mid_changed,
               "cancel-only mid-price change can trigger a sample")) {
        return false;
    }
    return true;
}

bool test_failure_freeze_and_post_failure_cache() {
    sz_hp::SamplerConfig config = test_config();
    config.capture_failure_digest = true;
    sz_hp::InstrumentState state("000001.SZ", config);
    if (!seed_book(&state)) {
        return false;
    }
    const sz_hp::SampleDecision failed =
        state.process_trade(make_trade(3, 34200002, 99, 98, 10.0, 1));
    if (!check(!failed.ready && !state.available() &&
                   failed.reason == sz_hp::SampleBlockReason::kUnavailable &&
                   !state.pre_failure_digest().empty(),
               "failed fill freezes state and captures its pre-failure digest")) {
        return false;
    }
    const std::string frozen_book = state.book().digest();
    if (!check(state.process_order(make_order(4, 34200003, 4, true, 9.9, 10)),
               "post-failure order follows HP cache semantics")) {
        return false;
    }
    const sz_hp::SampleDecision suppressed =
        state.process_trade(make_trade(5, 34200004, 4, 2, 10.0, 1));
    if (!check(!suppressed.ready && suppressed.reason == sz_hp::SampleBlockReason::kUnavailable &&
                   state.book().digest() == frozen_book &&
                   state.queued_order_count() == 1 && state.queued_trade_count() == 2 &&
                   state.last_application_sequence() == 4 &&
                   state.cumulative_fill_amount() == 20.0,
               "post-failure events update HP caches but cannot mutate or sample the book")) {
        return false;
    }
    return true;
}

bool test_trade_trigger_uses_snapshot_timestamp() {
    sz_hp::SamplerConfig config = test_config();
    config.history_amount_threshold = 1000000000.0;
    sz_hp::InstrumentState state("000001.SZ", config);
    if (!seed_book(&state)) {
        return false;
    }
    state.process_observation(make_observation(34200000, 1000, 1000));
    state.process_trade(make_trade(3, 34200001, 1, 2, 10.0, 5));
    const sz_hp::SampleDecision incomplete =
        state.process_observation(make_observation(34200001, 1100, 1101, 9.9, 10.1));
    if (!check(!incomplete.ready &&
                   incomplete.reason == sz_hp::SampleBlockReason::kTurnoverIncomplete,
               "snapshot with incomplete turnover waits for the remaining trade")) {
        return false;
    }
    const sz_hp::SampleDecision ready =
        state.process_trade(make_trade(4, 34200002, 1, 2, 10.2, 5));
    if (!check(ready.ready && ready.trigger == sz_hp::SampleTrigger::kTrade &&
                   ready.event_time_ms == 34200001,
               "trade-triggered sample keeps the HP snapshot timestamp")) {
        return false;
    }
    return true;
}

bool test_adapter_rejection_suppresses_instrument() {
    sz_hp::SamplerConfig config = test_config();
    config.capture_failure_digest = true;
    sz_hp::InstrumentState state("000001.SZ", config);
    if (!seed_book(&state)) {
        return false;
    }
    state.reject_event(77, "invalid order time");
    if (!check(!state.available() && state.book().failure_sequence() == 77 &&
                   state.book().failure_reason() == "invalid order time" &&
                   !state.pre_failure_digest().empty(),
               "adapter rejection freezes the instrument and captures its digest")) {
        return false;
    }
    const sz_hp::SampleDecision suppressed =
        state.process_observation(make_observation(34200001, 1100, 1000));
    if (!check(!suppressed.ready &&
                   suppressed.reason == sz_hp::SampleBlockReason::kUnavailable,
               "adapter rejection suppresses subsequent samples")) {
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!test_queue_and_sample() || !test_turnover_and_invalid_observation() ||
        !test_afternoon_and_cancel_only() || !test_failure_freeze_and_post_failure_cache() ||
        !test_trade_trigger_uses_snapshot_timestamp() ||
        !test_adapter_rejection_suppresses_instrument()) {
        return 1;
    }
    std::cout << "sz_hp_realtime_state_test: PASS\n";
    return 0;
}
