#include "sze/factors/sz_hp_factor_adapter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sz_hp {

namespace {

inline double clamp_value(double value, double minimum, double maximum) {
    return std::max(minimum, std::min(maximum, value));
}

inline bool level_valid(const MarketObservation& observation, size_t index) {
    return observation.bid_volume[index] != 0.0 && observation.ask_volume[index] != 0.0;
}

inline double mid_price(const MarketObservation& observation) {
    return level_valid(observation, 0)
               ? (observation.ask_price[0] + observation.bid_price[0]) / 2.0
               : observation.last_price;
}

inline double spread(const MarketObservation& observation) {
    return level_valid(observation, 0)
               ? observation.ask_price[0] - observation.bid_price[0]
               : 0.0;
}

double quote_volume_sum(const std::array<double, 10>& volume, size_t count) {
    double result = 0.0;
    const size_t limit = std::min(count, volume.size());
    for (size_t i = 0; i < limit; ++i) {
        result += volume[i];
    }
    return result;
}

double weighted_side_distance(const MarketObservation& observation, bool ask_side) {
    const std::array<double, 10>& volume = ask_side ? observation.ask_volume
                                                    : observation.bid_volume;
    const std::array<double, 10>& price = ask_side ? observation.ask_price
                                                   : observation.bid_price;
    const double quote_sum = quote_volume_sum(volume, 5);
    if (quote_sum == 0.0) {
        return 0.0;
    }
    double weighted = 0.0;
    for (size_t i = 0; i < 5; ++i) {
        if (volume[i] == 0.0) {
            break;
        }
        weighted += volume[i] * price[i];
    }
    const double weighted_price = weighted / quote_sum;
    return ask_side ? weighted_price - mid_price(observation)
                    : mid_price(observation) - weighted_price;
}

double weighted_price(const MarketObservation& observation, size_t count) {
    double volume_sum = 0.0;
    double amount_sum = 0.0;
    const size_t limit = std::min(count, static_cast<size_t>(5));
    for (size_t i = 0; i < limit; ++i) {
        volume_sum += observation.ask_volume[i] + observation.bid_volume[i];
        if (observation.ask_volume[i] != 0.0) {
            amount_sum += observation.ask_price[i] * observation.ask_volume[i];
        }
        if (observation.bid_volume[i] != 0.0) {
            amount_sum += observation.bid_price[i] * observation.bid_volume[i];
        }
    }
    return volume_sum == 0.0 ? mid_price(observation) : amount_sum / volume_sum;
}

double percent_hermes(const MarketObservation& observation,
                       double upper_price,
                       double lower_price) {
    const double mid = mid_price(observation);
    if (mid < 0.01 || mid > 1e6) {
        return 0.0;
    }
    double sum = 0.0;
    int divisor = 0;
    if (!level_valid(observation, 0)) {
        if (observation.last_price <= upper_price && observation.last_price >= lower_price) {
            sum = observation.last_price;
        }
        divisor = 1;
    } else {
        for (size_t i = 0; i < 5; ++i) {
            if (!level_valid(observation, i)) {
                break;
            }
            const int weight = 5 - static_cast<int>(i);
            const double cross_price =
                (observation.ask_price[i] * observation.bid_volume[i] +
                 observation.bid_price[i] * observation.ask_volume[i]) /
                (observation.bid_volume[i] + observation.ask_volume[i]);
            sum += cross_price * static_cast<double>(weight);
            divisor += weight;
        }
    }
    if (divisor == 0) {
        return 0.0;
    }
    return clamp_value((sum / static_cast<double>(divisor) / mid - 1.0) * 1000.0,
                       -5.0,
                       5.0);
}

double trade_sqrt_positive(const MarketObservation& previous,
                           const MarketObservation& current,
                           double fee_share) {
    const int64_t volume = static_cast<int64_t>(current.total_volume - previous.total_volume);
    if (volume == 0 || !level_valid(previous, 0) || !level_valid(current, 0) ||
        !(fee_share > 0.0)) {
        return 0.0;
    }
    const double average_trade_price =
        (current.turnover - previous.turnover) / static_cast<double>(volume);
    const double previous_spread = spread(previous);
    if (previous_spread == 0.0) {
        return 0.0;
    }
    double ratio = (average_trade_price - mid_price(previous)) / previous_spread;
    ratio = clamp_value(ratio, -0.5, 0.5);
    const double fee_scaled_volume = static_cast<double>(volume) / fee_share;
    return std::sqrt(fee_scaled_volume * (0.5 + ratio)) -
           std::sqrt(fee_scaled_volume * (0.5 - ratio));
}

void build_market_values(const MarketObservation& previous,
                         const MarketObservation& current,
                         const SamplerConfig& config,
                         std::array<float, kHpMarketFactorCount>* destination) {
    if (destination == 0) {
        return;
    }
    destination->fill(0.0f);
    const double current_mid = mid_price(current);
    const double previous_mid = mid_price(previous);
    const double upper_price = current.upper_limit_price > 0.0
                                   ? current.upper_limit_price
                                   : config.upper_price;
    const double lower_price = current.lower_limit_price > 0.0
                                   ? current.lower_limit_price
                                   : config.lower_price;
    (*destination)[0] = static_cast<float>(
        percent_hermes(current, upper_price, lower_price));
    (*destination)[1] = static_cast<float>(
        trade_sqrt_positive(previous, current, config.fee_share));
    (*destination)[2] = current_mid == 0.0
                            ? 0.0f
                            : static_cast<float>(spread(current) / current_mid * 1000.0);
    (*destination)[3] = current_mid == 0.0
                            ? 0.0f
                            : static_cast<float>((current_mid - previous_mid) /
                                                 current_mid * 1000.0);
    (*destination)[4] = current.last_price < 0.0
                            ? 0.0f
                            : static_cast<float>(std::sqrt(0.15 * current.last_price));

    const double current_top_volume = current.ask_volume[0] + current.bid_volume[0];
    if (current_top_volume != 0.0) {
        double bid_change = 0.0;
        if (current.bid_price[0] - previous.bid_price[0] < -1e-6) {
            bid_change = -previous.bid_volume[0] / current_top_volume;
        } else if (current.bid_price[0] - previous.bid_price[0] > 1e-6) {
            bid_change = (previous.ask_volume[0] + current.bid_volume[0]) /
                         current_top_volume;
        } else {
            bid_change = (current.bid_volume[0] - previous.bid_volume[0]) /
                         current_top_volume;
        }
        double ask_change = 0.0;
        if (current.ask_price[0] - previous.ask_price[0] < -1e-6) {
            ask_change = (previous.bid_volume[0] + current.ask_volume[0]) /
                         current_top_volume;
        } else if (current.ask_price[0] - previous.ask_price[0] > 1e-6) {
            ask_change = -previous.ask_volume[0] / current_top_volume;
        } else {
            ask_change = (current.ask_volume[0] - previous.ask_volume[0]) /
                         current_top_volume;
        }
        (*destination)[5] = static_cast<float>(clamp_value(bid_change, -200.0, 200.0));
        (*destination)[6] = static_cast<float>(clamp_value(ask_change, -200.0, 200.0));
    }

    for (size_t level = 1; level <= 5; ++level) {
        (*destination)[6 + level] = current_mid == 0.0
                                        ? 0.0f
                                        : static_cast<float>(
                                              (weighted_price(current, level) -
                                               weighted_price(previous, level)) /
                                              current_mid * 1000.0);
    }
    (*destination)[12] = current_mid == 0.0
                             ? 0.0f
                             : static_cast<float>(weighted_side_distance(current, true) /
                                                  current_mid * 1000.0);
    (*destination)[13] = current_mid == 0.0
                             ? 0.0f
                             : static_cast<float>(weighted_side_distance(current, false) /
                                                  current_mid * 1000.0);
    (*destination)[14] = previous_mid == 0.0
                             ? 0.0f
                             : static_cast<float>((weighted_side_distance(current, true) -
                                                   weighted_side_distance(previous, true)) /
                                                  previous_mid * 1000.0);
    (*destination)[15] = previous_mid == 0.0
                             ? 0.0f
                             : static_cast<float>((weighted_side_distance(current, false) -
                                                   weighted_side_distance(previous, false)) /
                                                  previous_mid * 1000.0);

    double weighted_ask_volume = 0.0;
    double weighted_bid_volume = 0.0;
    for (size_t i = 0; i < 5; ++i) {
        const double weight = static_cast<double>(5 - i);
        weighted_ask_volume += weight * current.ask_volume[i];
        weighted_bid_volume += weight * current.bid_volume[i];
    }
    const double weighted_total = weighted_ask_volume + weighted_bid_volume;
    (*destination)[16] = weighted_total == 0.0
                             ? 0.0f
                             : static_cast<float>(weighted_ask_volume / weighted_total - 0.5);
    const double ask_volume = quote_volume_sum(current.ask_volume, 5);
    const double bid_volume = quote_volume_sum(current.bid_volume, 5);
    const double total_volume = ask_volume + bid_volume;
    (*destination)[17] = total_volume == 0.0
                             ? 0.0f
                             : static_cast<float>(ask_volume / total_volume - 0.5);

    double visible_turnover = 0.0;
    for (size_t i = 0; i < 5; ++i) {
        if (current.ask_volume[i] != 0.0) {
            visible_turnover += current.ask_price[i] * current.ask_volume[i];
        }
        if (current.bid_volume[i] != 0.0) {
            visible_turnover += current.bid_price[i] * current.bid_volume[i];
        }
    }
    (*destination)[18] = visible_turnover == 0.0
                             ? 0.0f
                             : static_cast<float>((current.turnover - previous.turnover) /
                                                  visible_turnover);
    (*destination)[19] = ask_volume == 0.0
                             ? 0.0f
                             : static_cast<float>(current.ask_volume[0] / ask_volume);
    (*destination)[20] = bid_volume == 0.0
                             ? 0.0f
                             : static_cast<float>(current.bid_volume[0] / bid_volume);
}

void append_full_book_values(const ShSzFullOrderBookFactorSet& factors,
                             std::array<float, kHpOrderTradeFullBookFactorCount>* values) {
    if (values == 0) {
        return;
    }
    (*values)[7] = factors.PositiveFillRate;
    (*values)[8] = factors.NegativeFillRate;
    (*values)[9] = factors.OrderFlowImbalance;
    (*values)[10] = factors.CFRImbalance;
    (*values)[11] = factors.FixDisImbalancePct1;
    (*values)[12] = factors.FixDisImbalancePct2;
    (*values)[13] = factors.WeightedFixDisImbalancePct1;
    (*values)[14] = factors.WeightedFixDisImbalancePct2;
    (*values)[15] = factors.AvgSizeImbalance;
    (*values)[16] = factors.AvgSizeImbalanceLevel1;
    (*values)[17] = factors.AvgSizeImbalanceLevel5;
    (*values)[18] = factors.OrderCountImbalance;
    (*values)[19] = factors.OrderCountImbalanceLevel1;
    (*values)[20] = factors.OrderCountImbalanceLevel5;
    (*values)[21] = factors.OrderLifeImbalance;
    (*values)[22] = factors.OrderLifeImbalanceLevel1;
    (*values)[23] = factors.OrderLifeImbalanceLevel5;
    (*values)[24] = factors.MaxBidDistance;
    (*values)[25] = factors.MaxAskDistance;
    (*values)[26] = factors.MaxVolDistanceImbalance;
    (*values)[27] = factors.YoungOrderbookImbalance;
    (*values)[28] = factors.FixDistHermes;
}

void append_cob_full_book_values(const ShSzFullOrderBookFactorSet& factors,
                                 std::array<float, kHpCobFactorCount>* values) {
    if (values == 0) {
        return;
    }
    const size_t offset = kHpOrderTradeFactorCount + kHpMarketFactorCount;
    (*values)[offset + 0] = factors.PositiveFillRate;
    (*values)[offset + 1] = factors.NegativeFillRate;
    (*values)[offset + 2] = factors.OrderFlowImbalance;
    (*values)[offset + 3] = factors.CFRImbalance;
    (*values)[offset + 4] = factors.FixDisImbalancePct1;
    (*values)[offset + 5] = factors.FixDisImbalancePct2;
    (*values)[offset + 6] = factors.WeightedFixDisImbalancePct1;
    (*values)[offset + 7] = factors.WeightedFixDisImbalancePct2;
    (*values)[offset + 8] = factors.AvgSizeImbalance;
    (*values)[offset + 9] = factors.AvgSizeImbalanceLevel1;
    (*values)[offset + 10] = factors.AvgSizeImbalanceLevel5;
    (*values)[offset + 11] = factors.OrderCountImbalance;
    (*values)[offset + 12] = factors.OrderCountImbalanceLevel1;
    (*values)[offset + 13] = factors.OrderCountImbalanceLevel5;
    (*values)[offset + 14] = factors.OrderLifeImbalance;
    (*values)[offset + 15] = factors.OrderLifeImbalanceLevel1;
    (*values)[offset + 16] = factors.OrderLifeImbalanceLevel5;
    (*values)[offset + 17] = factors.MaxBidDistance;
    (*values)[offset + 18] = factors.MaxAskDistance;
    (*values)[offset + 19] = factors.MaxVolDistanceImbalance;
    (*values)[offset + 20] = factors.YoungOrderbookImbalance;
    (*values)[offset + 21] = factors.FixDistHermes;
}

}  // namespace

HpOrderTradeFullBookVector::HpOrderTradeFullBookVector() : values(), valid(false) {
    values.fill(0.0f);
}

HpCobFactorVector::HpCobFactorVector()
    : values(), model_values(), market_values(), valid(false) {
    values.fill(0.0f);
    model_values.fill(0.0f);
    market_values.fill(0.0f);
}

FactorInput::FactorInput()
    : raw_order_flow(), full_orderbook(), ordered_values(), cob_values(),
      event_time_ms(0), valid(false) {
}

ShSzOrderFlowSummary to_shsz_order_flow(const OrderFlowSample& sample) {
    ShSzOrderFlowSummary result;
    result.order_pf = sample.order_pf;
    result.order_nf = sample.order_nf;
    result.order_mf = sample.order_mf;
    result.trade_pcf = sample.raw_trade_pcf;
    result.trade_ncf = sample.raw_trade_ncf;
    result.trade_pt = sample.raw_trade_pt;
    result.trade_nt = sample.raw_trade_nt;
    result.cxl_buy_flow = sample.cxl_buy_flow;
    result.cxl_sell_flow = sample.cxl_sell_flow;
    result.buy_order_volume = sample.buy_order_volume;
    result.sell_order_volume = sample.sell_order_volume;
    return result;
}

FactorInput build_factor_input(const InstrumentState& state, const SampleBatch& batch) {
    FactorInput result;
    if (!batch.valid || !state.available()) {
        return result;
    }
    result.raw_order_flow = to_shsz_order_flow(batch.order_flow);
    result.event_time_ms = batch.current_observation.event_time_ms;
    result.full_orderbook = ShSzFullOrderBookFactorExtractor::build_predictor_input(
        state.book(),
        result.raw_order_flow,
        result.event_time_ms,
        state.config().fee_share);

    result.ordered_values.values[0] = static_cast<float>(batch.order_flow.order_pf);
    result.ordered_values.values[1] = static_cast<float>(batch.order_flow.order_nf);
    result.ordered_values.values[2] = static_cast<float>(batch.order_flow.order_mf);
    result.ordered_values.values[3] = static_cast<float>(batch.order_flow.trade_pcf);
    result.ordered_values.values[4] = static_cast<float>(batch.order_flow.trade_ncf);
    result.ordered_values.values[5] = static_cast<float>(batch.order_flow.trade_pt);
    result.ordered_values.values[6] = static_cast<float>(batch.order_flow.trade_nt);
    append_full_book_values(result.full_orderbook.factors, &result.ordered_values.values);
    result.ordered_values.valid = result.full_orderbook.valid;
    result.cob_values.values[0] = result.ordered_values.values[0];
    result.cob_values.values[1] = result.ordered_values.values[1];
    result.cob_values.values[2] = result.ordered_values.values[2];
    result.cob_values.values[3] = result.ordered_values.values[3];
    result.cob_values.values[4] = result.ordered_values.values[4];
    result.cob_values.values[5] = result.ordered_values.values[5];
    result.cob_values.values[6] = result.ordered_values.values[6];
    build_market_values(batch.previous_observation,
                        batch.current_observation,
                        state.config(),
                        &result.cob_values.market_values);
    for (size_t i = 0; i < result.cob_values.market_values.size(); ++i) {
        result.cob_values.values[kHpOrderTradeFactorCount + i] =
            result.cob_values.market_values[i];
    }
    append_cob_full_book_values(result.full_orderbook.factors, &result.cob_values.values);
    for (size_t i = 0; i < kHpMarketFactorCount; ++i) {
        result.cob_values.model_values[i] = result.cob_values.market_values[i];
    }
    for (size_t i = 0; i < kHpOrderTradeFactorCount; ++i) {
        result.cob_values.model_values[kHpMarketFactorCount + i] =
            result.cob_values.values[i];
    }
    for (size_t i = 0; i < kHpFullOrderBookFactorCount; ++i) {
        result.cob_values.model_values[kHpMarketFactorCount +
                                       kHpOrderTradeFactorCount + i] =
            result.cob_values.values[kHpOrderTradeFactorCount +
                                     kHpMarketFactorCount + i];
    }
    result.cob_values.valid = result.full_orderbook.valid &&
                              batch.previous_observation.valid &&
                              batch.current_observation.valid;
    result.valid = result.ordered_values.valid && result.cob_values.valid;
    return result;
}

const std::array<const char*, kHpCobFactorCount>& hp_cob_factor_names() {
    static const std::array<const char*, kHpCobFactorCount> names = {{
        "order_pf", "order_nf", "order_mf", "trade_pcf", "trade_ncf",
        "trade_pt", "trade_nt", "hermes", "tr_sqrt_positive",
        "percent_spread", "percent_mid_rtn", "fee_on_tick",
        "bid_vol_chg_ratio", "ask_vol_chg_ratio", "pct_weighted_rtn1",
        "pct_weighted_rtn2", "pct_weighted_rtn3", "pct_weighted_rtn4",
        "pct_weighted_rtn5", "pct_weighted_ask", "pct_weighted_bid",
        "pct_weighted_ask_rtn", "pct_weighted_bid_rtn",
        "weighted_vol_diff_ratio", "vol_diff_ratio", "pct_turnover",
        "pct_liquidity_ask", "pct_liquidity_bid", "PositiveFillRate",
        "NegativeFillRate", "OrderFlowImbalance", "CFRImbalance",
        "FixDisImbalancePct1", "FixDisImbalancePct2",
        "WeightedFixDisImbalancePct1", "WeightedFixDisImbalancePct2",
        "AvgSizeImbalance", "AvgSizeImbalanceLevel1",
        "AvgSizeImbalanceLevel5", "OrderCountImbalance",
        "OrderCountImbalanceLevel1", "OrderCountImbalanceLevel5",
        "OrderLifeImbalance", "OrderLifeImbalanceLevel1",
        "OrderLifeImbalanceLevel5", "MaxBidDistance", "MaxAskDistance",
        "MaxVolDistanceImbalance", "YoungOrderbookImbalance",
        "FixDistHermes"
    }};
    return names;
}

const std::array<const char*, kHpCobFactorCount>& hp_cob_model_input_names() {
    static const std::array<const char*, kHpCobFactorCount> names = {{
        "hermes", "tr_sqrt_positive", "percent_spread", "percent_mid_rtn",
        "fee_on_tick", "bid_vol_chg_ratio", "ask_vol_chg_ratio",
        "pct_weighted_rtn1", "pct_weighted_rtn2", "pct_weighted_rtn3",
        "pct_weighted_rtn4", "pct_weighted_rtn5", "pct_weighted_ask",
        "pct_weighted_bid", "pct_weighted_ask_rtn", "pct_weighted_bid_rtn",
        "weighted_vol_diff_ratio", "vol_diff_ratio", "pct_turnover",
        "pct_liquidity_ask", "pct_liquidity_bid", "order_pf", "order_nf",
        "order_mf", "trade_pcf", "trade_ncf", "trade_pt", "trade_nt",
        "PositiveFillRate", "NegativeFillRate", "OrderFlowImbalance",
        "CFRImbalance", "FixDisImbalancePct1", "FixDisImbalancePct2",
        "WeightedFixDisImbalancePct1", "WeightedFixDisImbalancePct2",
        "AvgSizeImbalance", "AvgSizeImbalanceLevel1",
        "AvgSizeImbalanceLevel5", "OrderCountImbalance",
        "OrderCountImbalanceLevel1", "OrderCountImbalanceLevel5",
        "OrderLifeImbalance", "OrderLifeImbalanceLevel1",
        "OrderLifeImbalanceLevel5", "MaxBidDistance", "MaxAskDistance",
        "MaxVolDistanceImbalance", "YoungOrderbookImbalance",
        "FixDistHermes"
    }};
    return names;
}

bool normalize_cob_model_input(
    const HpCobFactorVector& factors,
    const std::array<float, kHpCobFactorCount>& mean,
    const std::array<float, kHpCobFactorCount>& std,
    std::array<float, kHpCobFactorCount>* output) {
    if (output == 0 || !factors.valid) {
        return false;
    }
    for (size_t i = 0; i < kHpCobFactorCount; ++i) {
        if (!std::isfinite(static_cast<double>(factors.model_values[i])) ||
            !std::isfinite(static_cast<double>(mean[i])) ||
            !std::isfinite(static_cast<double>(std[i])) || std[i] == 0.0f) {
            return false;
        }
        (*output)[i] = (factors.model_values[i] - mean[i]) / std[i];
    }
    return true;
}

}  // namespace sz_hp
