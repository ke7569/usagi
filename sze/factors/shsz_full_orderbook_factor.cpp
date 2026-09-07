#include "sze/factors/shsz_full_orderbook_factor.h"

#include "common/market_data/legacy/shsz_full_orderbook_diagnostics.h"

namespace {

inline double clamp_double(double value, double min_value, double max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

inline double positive_fill_rate(const ShSzOrderFlowSummary& order_flow) {
    return order_flow.buy_order_volume > 1e-6
               ? clamp_double(order_flow.trade_pt / order_flow.buy_order_volume, 0.0, 5.0)
               : 0.0;
}

inline double negative_fill_rate(const ShSzOrderFlowSummary& order_flow) {
    return order_flow.sell_order_volume > 1e-6
               ? clamp_double(order_flow.trade_nt / order_flow.sell_order_volume, 0.0, 5.0)
               : 0.0;
}

inline double order_flow_imbalance(const ShSzOrderFlowSummary& order_flow) {
    const double buy_order_volume = order_flow.buy_order_volume;
    const double sell_order_volume = order_flow.sell_order_volume;
    return (buy_order_volume - sell_order_volume) / (buy_order_volume + sell_order_volume + 1.0);
}

inline double cfr_imbalance(const ShSzOrderFlowSummary& order_flow) {
    const double buy_cfr = order_flow.trade_pt / (order_flow.trade_pt + order_flow.cxl_buy_flow + 1.0);
    const double sell_cfr = order_flow.trade_nt / (order_flow.trade_nt + order_flow.cxl_sell_flow + 1.0);
    return (buy_cfr - sell_cfr) / (buy_cfr + sell_cfr + 1.0);
}

inline double fix_dis_imbalance(const ShSzFullOrderSum& ask_sum, const ShSzFullOrderSum& bid_sum) {
    return (ask_sum.volume_sum - bid_sum.volume_sum) / (ask_sum.volume_sum + bid_sum.volume_sum + 1.0);
}

inline double weighted_fix_dis_imbalance(const ShSzFullOrderSum& ask_sum,
                                         const ShSzFullOrderSum& bid_sum,
                                         double mp,
                                         double max_distance) {
    if (mp <= 0.0 || max_distance <= 0.0) {
        return 0.0;
    }
    const double ask_weighted =
        ask_sum.volume_sum * (1.0 + mp / max_distance) - ask_sum.amt_sum / max_distance;
    const double bid_weighted =
        bid_sum.volume_sum * (1.0 - mp / max_distance) + bid_sum.amt_sum / max_distance;
    if (ask_weighted + bid_weighted == 0.0) {
        return 0.0;
    }
    return (ask_weighted - bid_weighted) / (ask_weighted + bid_weighted);
}

inline double avg_size_imbalance(const ShSzFullOrderSum& ask_sum, const ShSzFullOrderSum& bid_sum) {
    if (ask_sum.count_sum == 0 || bid_sum.count_sum == 0) {
        return 0.0;
    }
    const double bid_avg_size = bid_sum.volume_sum / static_cast<double>(bid_sum.count_sum);
    const double ask_avg_size = ask_sum.volume_sum / static_cast<double>(ask_sum.count_sum);
    if (ask_avg_size + bid_avg_size == 0.0) {
        return 0.0;
    }
    return (ask_avg_size - bid_avg_size) / (ask_avg_size + bid_avg_size);
}

inline double order_count_imbalance(const ShSzFullOrderSum& ask_sum, const ShSzFullOrderSum& bid_sum) {
    if (ask_sum.count_sum + bid_sum.count_sum == 0) {
        return 0.0;
    }
    return static_cast<double>(ask_sum.count_sum - bid_sum.count_sum) /
           static_cast<double>(ask_sum.count_sum + bid_sum.count_sum);
}

inline double order_life_imbalance(const ShSzFullOrderSum& ask_sum,
                                   const ShSzFullOrderSum& bid_sum,
                                   int64_t cur_tsc) {
    if (ask_sum.count_sum == 0 || bid_sum.count_sum == 0) {
        return 0.0;
    }

    const double ask_life = static_cast<double>(cur_tsc) -
                            static_cast<double>(ask_sum.tsc_sum) / static_cast<double>(ask_sum.count_sum);
    const double bid_life = static_cast<double>(cur_tsc) -
                            static_cast<double>(bid_sum.tsc_sum) / static_cast<double>(bid_sum.count_sum);
    if (ask_life + bid_life == 0.0) {
        return 0.0;
    }
    return (ask_life - bid_life) / (ask_life + bid_life);
}

inline double max_vol_distance_imbalance(double max_ask_price, double max_bid_price, double mp) {
    if (max_ask_price <= 0.0 || max_bid_price <= 0.0 || mp <= 0.0) {
        return 0.0;
    }
    const double ask_distance = max_ask_price / mp - 1.0;
    const double bid_distance = -(max_bid_price / mp - 1.0);
    if (ask_distance + bid_distance == 0.0) {
        return 0.0;
    }
    return (ask_distance - bid_distance) / (ask_distance + bid_distance);
}

template <typename Book>
ShSzFullOrderBookFactorSet extract_with_aggregate(const Book& engine,
                                                  const ShSzOrderFlowSummary& order_flow,
                                                  const ShSzFullOb& full_ob,
                                                  uint32_t now_time_ms,
                                                  double fee_share) {
    (void)fee_share;

    const bool latency_enabled = shsz_full_orderbook_latency_enabled();
    ShSzFullOrderBookLatencyStats* latency_stats =
        latency_enabled ? &shsz_full_orderbook_latency_stats() : 0;

    ShSzFullOrderBookFactorSet factor_set;
    uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.PositiveFillRate = static_cast<float>(positive_fill_rate(order_flow));
    if (latency_stats != 0) {
        latency_stats->positive_fill_rate_count += 1;
        latency_stats->positive_fill_rate_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.NegativeFillRate = static_cast<float>(negative_fill_rate(order_flow));
    if (latency_stats != 0) {
        latency_stats->negative_fill_rate_count += 1;
        latency_stats->negative_fill_rate_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.OrderFlowImbalance = static_cast<float>(order_flow_imbalance(order_flow));
    if (latency_stats != 0) {
        latency_stats->order_flow_imbalance_count += 1;
        latency_stats->order_flow_imbalance_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.CFRImbalance = static_cast<float>(cfr_imbalance(order_flow));
    if (latency_stats != 0) {
        latency_stats->cfr_imbalance_count += 1;
        latency_stats->cfr_imbalance_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }

    if (!full_ob.valid || full_ob.ask_total_count == 0 || full_ob.bid_total_count == 0 || full_ob.mp <= 0.0) {
        return factor_set;
    }

    factor_set.valid = true;
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.FixDisImbalancePct1 = static_cast<float>(fix_dis_imbalance(full_ob.ask_01, full_ob.bid_01));
    if (latency_stats != 0) {
        latency_stats->fix_dis_imbalance_pct1_count += 1;
        latency_stats->fix_dis_imbalance_pct1_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.FixDisImbalancePct2 = static_cast<float>(fix_dis_imbalance(full_ob.ask_05, full_ob.bid_05));
    if (latency_stats != 0) {
        latency_stats->fix_dis_imbalance_pct2_count += 1;
        latency_stats->fix_dis_imbalance_pct2_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.WeightedFixDisImbalancePct1 =
        static_cast<float>(weighted_fix_dis_imbalance(full_ob.ask_01, full_ob.bid_01, full_ob.mp, full_ob.mp * 0.01));
    if (latency_stats != 0) {
        latency_stats->weighted_fix_dis_imbalance_pct1_count += 1;
        latency_stats->weighted_fix_dis_imbalance_pct1_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.WeightedFixDisImbalancePct2 =
        static_cast<float>(weighted_fix_dis_imbalance(full_ob.ask_05, full_ob.bid_05, full_ob.mp, full_ob.mp * 0.05));
    if (latency_stats != 0) {
        latency_stats->weighted_fix_dis_imbalance_pct2_count += 1;
        latency_stats->weighted_fix_dis_imbalance_pct2_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.AvgSizeImbalance = static_cast<float>(avg_size_imbalance(full_ob.ask_10, full_ob.bid_10));
    if (latency_stats != 0) {
        latency_stats->avg_size_imbalance_count += 1;
        latency_stats->avg_size_imbalance_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.AvgSizeImbalanceLevel1 = static_cast<float>(avg_size_imbalance(full_ob.ask_level1, full_ob.bid_level1));
    if (latency_stats != 0) {
        latency_stats->avg_size_imbalance_level1_count += 1;
        latency_stats->avg_size_imbalance_level1_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.AvgSizeImbalanceLevel5 = static_cast<float>(avg_size_imbalance(full_ob.ask_level5, full_ob.bid_level5));
    if (latency_stats != 0) {
        latency_stats->avg_size_imbalance_level5_count += 1;
        latency_stats->avg_size_imbalance_level5_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.OrderCountImbalance = static_cast<float>(order_count_imbalance(full_ob.ask_10, full_ob.bid_10));
    if (latency_stats != 0) {
        latency_stats->order_count_imbalance_count += 1;
        latency_stats->order_count_imbalance_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.OrderCountImbalanceLevel1 =
        static_cast<float>(order_count_imbalance(full_ob.ask_level1, full_ob.bid_level1));
    if (latency_stats != 0) {
        latency_stats->order_count_imbalance_level1_count += 1;
        latency_stats->order_count_imbalance_level1_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.OrderCountImbalanceLevel5 =
        static_cast<float>(order_count_imbalance(full_ob.ask_level5, full_ob.bid_level5));
    if (latency_stats != 0) {
        latency_stats->order_count_imbalance_level5_count += 1;
        latency_stats->order_count_imbalance_level5_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.OrderLifeImbalance =
        static_cast<float>(order_life_imbalance(full_ob.ask_10, full_ob.bid_10, static_cast<int64_t>(now_time_ms)));
    if (latency_stats != 0) {
        latency_stats->order_life_imbalance_count += 1;
        latency_stats->order_life_imbalance_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.OrderLifeImbalanceLevel1 = static_cast<float>(
        order_life_imbalance(full_ob.ask_level1, full_ob.bid_level1, static_cast<int64_t>(now_time_ms)));
    if (latency_stats != 0) {
        latency_stats->order_life_imbalance_level1_count += 1;
        latency_stats->order_life_imbalance_level1_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.OrderLifeImbalanceLevel5 = static_cast<float>(
        order_life_imbalance(full_ob.ask_level5, full_ob.bid_level5, static_cast<int64_t>(now_time_ms)));
    if (latency_stats != 0) {
        latency_stats->order_life_imbalance_level5_count += 1;
        latency_stats->order_life_imbalance_level5_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.MaxBidDistance = static_cast<float>(full_ob.bid_max_level_price / full_ob.mp - 1.0);
    if (latency_stats != 0) {
        latency_stats->max_bid_distance_count += 1;
        latency_stats->max_bid_distance_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.MaxAskDistance = static_cast<float>(full_ob.ask_max_level_price / full_ob.mp - 1.0);
    if (latency_stats != 0) {
        latency_stats->max_ask_distance_count += 1;
        latency_stats->max_ask_distance_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.MaxVolDistanceImbalance = static_cast<float>(
        max_vol_distance_imbalance(full_ob.ask_max_level_price, full_ob.bid_max_level_price, full_ob.mp));
    if (latency_stats != 0) {
        latency_stats->max_vol_distance_imbalance_count += 1;
        latency_stats->max_vol_distance_imbalance_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.YoungOrderbookImbalance = static_cast<float>(engine.young_orderbook_imbalance(100, now_time_ms));
    if (latency_stats != 0) {
        latency_stats->young_orderbook_imbalance_count += 1;
        latency_stats->young_orderbook_imbalance_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor_set.FixDistHermes = static_cast<float>(engine.fix_dist_hermes(500, now_time_ms));
    if (latency_stats != 0) {
        latency_stats->fix_dist_hermes_count += 1;
        latency_stats->fix_dist_hermes_ns += (shsz_full_orderbook_now_ns() - begin_ns);
    }
    return factor_set;
}

} // namespace

ShSzOrderFlowSummary::ShSzOrderFlowSummary()
    : order_pf(0.0),
      order_nf(0.0),
      order_mf(0.0),
      trade_pcf(0.0),
      trade_ncf(0.0),
      trade_pt(0.0),
      trade_nt(0.0),
      cxl_buy_flow(0.0),
      cxl_sell_flow(0.0),
      buy_order_volume(0.0),
      sell_order_volume(0.0) {
}

ShSzFullOrderBookFactorSet::ShSzFullOrderBookFactorSet()
    : PositiveFillRate(0.0f),
      NegativeFillRate(0.0f),
      OrderFlowImbalance(0.0f),
      CFRImbalance(0.0f),
      FixDisImbalancePct1(0.0f),
      FixDisImbalancePct2(0.0f),
      WeightedFixDisImbalancePct1(0.0f),
      WeightedFixDisImbalancePct2(0.0f),
      AvgSizeImbalance(0.0f),
      AvgSizeImbalanceLevel1(0.0f),
      AvgSizeImbalanceLevel5(0.0f),
      OrderCountImbalance(0.0f),
      OrderCountImbalanceLevel1(0.0f),
      OrderCountImbalanceLevel5(0.0f),
      OrderLifeImbalance(0.0f),
      OrderLifeImbalanceLevel1(0.0f),
      OrderLifeImbalanceLevel5(0.0f),
      MaxBidDistance(0.0f),
      MaxAskDistance(0.0f),
      MaxVolDistanceImbalance(0.0f),
      YoungOrderbookImbalance(0.0f),
      FixDistHermes(0.0f),
      valid(false) {
}

ShSzFullOrderBookPredictorInput::ShSzFullOrderBookPredictorInput()
    : order_flow(),
      aggregate(),
      factors(),
      now_time_ms(0),
      fee_share(0.0),
      valid(false) {
}

ShSzFullOrderBookFactorSet ShSzFullOrderBookFactorExtractor::extract(const ShSzFullOrderBookEngine& engine,
                                                                     const ShSzOrderFlowSummary& order_flow,
                                                                     uint32_t now_time_ms,
                                                                     double fee_share) {
    const ShSzFullOb full_ob = engine.snapshot_full_orderbook_aggregate(now_time_ms);
    return extract_with_aggregate(engine, order_flow, full_ob, now_time_ms, fee_share);
}

ShSzFullOrderBookFactorSet ShSzFullOrderBookFactorExtractor::extract(
    const sz_hp::OrderBook& book,
    const ShSzOrderFlowSummary& order_flow,
    uint32_t now_time_ms,
    double fee_share) {
    const ShSzFullOb full_ob = book.snapshot_full_orderbook_aggregate(now_time_ms);
    return extract_with_aggregate(book, order_flow, full_ob, now_time_ms, fee_share);
}

ShSzFullOrderBookPredictorInput ShSzFullOrderBookFactorExtractor::build_predictor_input(
    const ShSzFullOrderBookEngine& engine,
    const ShSzOrderFlowSummary& order_flow,
    uint32_t now_time_ms,
    double fee_share) {
    ShSzFullOrderBookPredictorInput input;
    input.order_flow = order_flow;
    input.aggregate = engine.snapshot_full_orderbook_aggregate(now_time_ms);
    input.factors = extract_with_aggregate(engine, order_flow, input.aggregate, now_time_ms, fee_share);
    input.now_time_ms = now_time_ms;
    input.fee_share = fee_share;
    input.valid = input.aggregate.valid && input.factors.valid;
    return input;
}

ShSzFullOrderBookPredictorInput ShSzFullOrderBookFactorExtractor::build_predictor_input(
    const sz_hp::OrderBook& book,
    const ShSzOrderFlowSummary& order_flow,
    uint32_t now_time_ms,
    double fee_share) {
    ShSzFullOrderBookPredictorInput input;
    input.order_flow = order_flow;
    input.aggregate = book.snapshot_full_orderbook_aggregate(now_time_ms);
    input.factors = extract_with_aggregate(book, order_flow, input.aggregate, now_time_ms, fee_share);
    input.now_time_ms = now_time_ms;
    input.fee_share = fee_share;
    input.valid = input.aggregate.valid && input.factors.valid;
    return input;
}

ShSzFullOrderBookPredictorInput ShSzFullOrderBookFactorExtractor::build_predictor_input(
    const ShSzFullOrderBookManager& manager,
    const char* instrument_id,
    const ShSzOrderFlowSummary& order_flow,
    uint32_t now_time_ms,
    double fee_share) {
    const ShSzFullOrderBookEngine* engine = manager.get_engine(instrument_id);
    if (engine == 0) {
        return ShSzFullOrderBookPredictorInput();
    }
    return build_predictor_input(*engine, order_flow, now_time_ms, fee_share);
}
