#include "sze/factors/sz_hp_factor_adapter.h"

#include <cstring>
#include <iostream>
#include <array>
#include <cmath>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

bool check_close(double actual, double expected, double tolerance, const char* message) {
    return check(std::fabs(actual - expected) <= tolerance, message);
}

sz_hp::MarketObservation observation(uint32_t time_ms,
                                     double volume,
                                     double turnover,
                                     double bid = 10.0,
                                     double ask = 10.1) {
    sz_hp::MarketObservation result;
    std::strncpy(result.instrument.data(), "000001.SZ", result.instrument.size() - 1);
    result.event_time_ms = time_ms;
    result.total_volume = volume;
    result.turnover = turnover;
    result.last_price = (bid + ask) / 2.0;
    result.bid_price[0] = bid;
    result.ask_price[0] = ask;
    result.bid_volume[0] = 100;
    result.ask_volume[0] = 100;
    result.valid = true;
    return result;
}

sz_hp::OrderEvent order(uint64_t sequence,
                        uint64_t id,
                        bool buy,
                        double price,
                        int64_t quantity) {
    sz_hp::OrderEvent result;
    std::strncpy(result.instrument.data(), "000001.SZ", result.instrument.size() - 1);
    result.sequence = sequence;
    result.order_id = id;
    result.event_time_ms = 34200000 + static_cast<uint32_t>(sequence);
    result.price = sz_hp::to_price(price);
    result.raw_price = price;
    result.quantity = quantity;
    result.is_buy = buy;
    result.type = sz_hp::OrderType::kLimitPrice;
    result.raw_order_type = '2';
    return result;
}

sz_hp::TradeEvent trade(uint64_t sequence,
                        uint64_t bid_id,
                        uint64_t ask_id,
                        double price,
                        int64_t quantity) {
    sz_hp::TradeEvent result;
    std::strncpy(result.instrument.data(), "000001.SZ", result.instrument.size() - 1);
    result.sequence = sequence;
    result.event_time_ms = 34200000 + static_cast<uint32_t>(sequence);
    result.bid_id = bid_id;
    result.ask_id = ask_id;
    result.price = sz_hp::to_price(price);
    result.raw_price = price;
    result.quantity = quantity;
    result.flag = sz_hp::TradeFlag::kFill;
    result.raw_trade_flag = 'F';
    return result;
}

bool test_factor_mapping() {
    sz_hp::SamplerConfig config;
    config.history_amount_threshold = 0.0;
    config.downsample = 1;
    config.minimum_volume_delta = 100.0;
    config.fee_share = 10.0;
    sz_hp::InstrumentState state("000001.SZ", config);
    if (!check(state.book().add_order(1, true, sz_hp::to_price(10.0), 100, 34200000) &&
                   state.book().add_order(2, false, sz_hp::to_price(10.1), 100, 34200001),
               "factor book setup")) {
        return false;
    }
    state.process_observation(observation(34200000, 1000, 1000));
    state.process_order(order(3, 3, true, 10.0, 10));
    state.process_trade(trade(4, 3, 2, 10.1, 10));
    if (!check(state.process_observation(observation(34200001, 1100, 1101)).ready,
               "factor sample setup")) {
        return false;
    }
    sz_hp::SampleBatch batch;
    if (!check(state.consume_sample(&batch), "factor sample consumed")) {
        return false;
    }
    const sz_hp::FactorInput input = sz_hp::build_factor_input(state, batch);
    if (!check(input.valid && input.full_orderbook.aggregate.valid,
               "HP book produces valid full-orderbook input")) {
        return false;
    }
    if (!check(input.raw_order_flow.buy_order_volume == 10.0 &&
                   input.raw_order_flow.trade_pt == 10.0,
               "raw order-flow fields preserve HP units")) {
        return false;
    }
    if (!check(input.ordered_values.values[0] == batch.order_flow.order_pf &&
                   input.ordered_values.values[5] == batch.order_flow.trade_pt &&
                   input.ordered_values.values[7] == input.full_orderbook.factors.PositiveFillRate &&
                   input.ordered_values.values[28] == input.full_orderbook.factors.FixDistHermes,
               "7 plus 22 factor order is deterministic")) {
        return false;
    }
    const std::array<float, sz_hp::kHpCobFactorCount> mean = {{0.0f}};
    std::array<float, sz_hp::kHpCobFactorCount> stddev;
    stddev.fill(1.0f);
    std::array<float, sz_hp::kHpCobFactorCount> normalized;
    if (!check(input.cob_values.valid &&
                   input.cob_values.model_values[0] == input.cob_values.values[7] &&
                   input.cob_values.model_values[21] == input.cob_values.values[0] &&
                   normalize_cob_model_input(input.cob_values, mean, stddev, &normalized) &&
                   normalized[0] == input.cob_values.values[7] &&
                   normalized[21] == input.cob_values.values[0],
               "COB model order is market, order/trade, full-book")) {
        return false;
    }
    const std::array<const char*, sz_hp::kHpCobFactorCount>& raw_names =
        sz_hp::hp_cob_factor_names();
    const std::array<const char*, sz_hp::kHpCobFactorCount>& model_names =
        sz_hp::hp_cob_model_input_names();
    if (!check(std::strcmp(raw_names[0], "order_pf") == 0 &&
                   std::strcmp(raw_names[7], "hermes") == 0 &&
                   std::strcmp(raw_names[28], "PositiveFillRate") == 0 &&
                   std::strcmp(model_names[0], "hermes") == 0 &&
                   std::strcmp(model_names[21], "order_pf") == 0 &&
                   std::strcmp(model_names[28], "PositiveFillRate") == 0,
               "HP factor names and indices are frozen")) {
        return false;
    }
    return true;
}

bool test_market_factor_formulas() {
    sz_hp::SamplerConfig config;
    config.history_amount_threshold = 0.0;
    config.downsample = 1;
    config.minimum_volume_delta = 100.0;
    config.fee_share = 10.0;
    sz_hp::InstrumentState state("000001.SZ", config);
    if (!check(state.book().add_order(1, true, sz_hp::to_price(10.0), 200, 34200000) &&
                   state.book().add_order(2, false, sz_hp::to_price(10.1), 200, 34200000),
               "market factor book setup")) {
        return false;
    }
    state.process_observation(observation(34200000, 1000, 1000));
    state.process_order(order(3, 3, true, 10.0, 100));
    state.process_trade(trade(4, 3, 2, 10.05, 100));
    if (!check(state.process_observation(observation(34200001, 1100, 2005)).ready,
               "market factor sample setup")) {
        return false;
    }
    sz_hp::SampleBatch batch;
    if (!check(state.consume_sample(&batch), "market factor sample consumed")) {
        return false;
    }
    const sz_hp::FactorInput input = sz_hp::build_factor_input(state, batch);
    const std::array<float, sz_hp::kHpMarketFactorCount>& values =
        input.cob_values.market_values;
    return check(input.valid, "market factor input valid") &&
           check_close(values[0], 0.0, 1e-6, "hermes") &&
           check_close(values[1], 0.0, 1e-6, "trade sqrt positive") &&
           check_close(values[2], 0.1 / 10.05 * 1000.0, 1e-4, "percent spread") &&
           check_close(values[3], 0.0, 1e-6, "percent mid return") &&
           check_close(values[5], 0.0, 1e-6, "bid volume change") &&
           check_close(values[6], 0.0, 1e-6, "ask volume change") &&
           check_close(values[12], 0.05 / 10.05 * 1000.0, 1e-4,
                       "percent weighted ask") &&
           check_close(values[13], 0.05 / 10.05 * 1000.0, 1e-4,
                       "percent weighted bid") &&
           check_close(values[16], 0.0, 1e-6, "weighted volume difference") &&
           check_close(values[17], 0.0, 1e-6, "volume difference") &&
           check_close(values[18], 0.5, 1e-6, "percent turnover") &&
           check_close(values[19], 1.0, 1e-6, "ask level-one liquidity") &&
           check_close(values[20], 1.0, 1e-6, "bid level-one liquidity");
}

}  // namespace

int main() {
    if (!test_factor_mapping() || !test_market_factor_formulas()) {
        return 1;
    }
    std::cout << "sz_hp_factor_adapter_test: PASS\n";
    return 0;
}
