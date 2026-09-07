#include "common/market_data/legacy/shsz_full_orderbook_diagnostics.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace {
bool g_shsz_full_orderbook_latency_enabled = false;

const char* const kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_FEATURE_COUNT] = {
    "positive_order_flow",
    "negative_order_flow",
    "market_order_flow",
    "cancel_buy_flow",
    "cancel_sell_flow",
    "positive_trade_flow",
    "negative_trade_flow",
    "positive_trade_num",
    "negative_trade_num"
};

const char* const kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_FEATURE_COUNT] = {
    "weighted_cross_price_rtn",
    "sqrt_trade_ratio",
    "spread",
    "mid_rtn",
    "tick_size",
    "bid_vol_change_ratio",
    "ask_vol_change_ratio",
    "weighted_rtn_lv1",
    "weighted_rtn_lv2",
    "weighted_rtn_lv3",
    "weighted_rtn_lv4",
    "weighted_rtn_lv5",
    "pct_weighted_ask",
    "pct_weighted_bid",
    "weighted_ask_rtn",
    "weighted_bid_rtn",
    "weighted_imabalance_lv5",
    "imbalance_lv5",
    "pct_turnover",
    "ask_pct_lv1",
    "bid_pct_lv1"
};

uint64_t avg_ns(uint64_t total, uint64_t count) {
    return count == 0 ? 0 : (total / count);
}

uint64_t combined_total(uint64_t lhs, uint64_t rhs) {
    return lhs + rhs;
}

uint64_t combined_count(uint64_t lhs, uint64_t rhs) {
    return lhs + rhs;
}
}

uint64_t shsz_full_orderbook_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool shsz_full_orderbook_latency_enabled() {
    static const bool env_enabled = []() {
        const char* env = std::getenv("T0_FULL_ORDERBOOK_LATENCY");
        return env != 0 && env[0] != '\0' && env[0] != '0';
    }();
    return g_shsz_full_orderbook_latency_enabled || env_enabled;
}

void shsz_full_orderbook_set_latency_enabled(bool enabled) {
    g_shsz_full_orderbook_latency_enabled = enabled;
}

ShSzFullOrderBookLatencyStats& shsz_full_orderbook_latency_stats() {
    static ShSzFullOrderBookLatencyStats stats;
    return stats;
}

uint64_t ShSzFullOrderBookLatencyStats::total_event_count() const {
    return process_order_count + process_trade_count;
}

void ShSzFullOrderBookLatencyStats::add_legacy_orderflow_sample(size_t index, uint64_t elapsed_ns) {
    if (index >= legacy_orderflow_count.size()) {
        return;
    }
    legacy_orderflow_count[index] += 1;
    legacy_orderflow_ns[index] += elapsed_ns;
}

void ShSzFullOrderBookLatencyStats::add_legacy_orderbook_sample(size_t index, uint64_t elapsed_ns) {
    if (index >= legacy_orderbook_count.size()) {
        return;
    }
    legacy_orderbook_count[index] += 1;
    legacy_orderbook_ns[index] += elapsed_ns;
}

std::string ShSzFullOrderBookLatencyStats::format_key_line() const {
    const uint64_t match_total_ns = combined_total(manager_order_ns, manager_trade_ns);

    std::ostringstream oss;
    oss << "[Timing][FullOrderBook][key]"
        << " event_time_ms=" << last_event_time_ms
        << " factor_total_ns=" << factor_ns
        << " factor_avg_ns=" << avg_ns(factor_ns, factor_count)
        << " factor_count=" << factor_count
        << " may_predict_count=" << may_predict_count
        << " do_predict_avg_ns=" << avg_ns(do_predict_ns, do_predict_count)
        << " do_predict_count=" << do_predict_count
        << " match_total_ns=" << match_total_ns
        << " match_event_count=" << combined_count(manager_order_count, manager_trade_count);
    return oss.str();
}

std::vector<std::string> ShSzFullOrderBookLatencyStats::format_factor_lines() const {
    std::vector<std::string> lines;
    std::ostringstream oss;

    oss << "[Timing][FullOrderBook][factor_core]"
        << " positive_fill_rate_avg_ns=" << avg_ns(positive_fill_rate_ns, positive_fill_rate_count)
        << " negative_fill_rate_avg_ns=" << avg_ns(negative_fill_rate_ns, negative_fill_rate_count)
        << " order_flow_imbalance_avg_ns=" << avg_ns(order_flow_imbalance_ns, order_flow_imbalance_count)
        << " cfr_imbalance_avg_ns=" << avg_ns(cfr_imbalance_ns, cfr_imbalance_count)
        << " fix_dis_imbalance_pct1_avg_ns=" << avg_ns(fix_dis_imbalance_pct1_ns, fix_dis_imbalance_pct1_count)
        << " fix_dis_imbalance_pct2_avg_ns=" << avg_ns(fix_dis_imbalance_pct2_ns, fix_dis_imbalance_pct2_count)
        << " weighted_fix_dis_imbalance_pct1_avg_ns="
        << avg_ns(weighted_fix_dis_imbalance_pct1_ns, weighted_fix_dis_imbalance_pct1_count)
        << " weighted_fix_dis_imbalance_pct2_avg_ns="
        << avg_ns(weighted_fix_dis_imbalance_pct2_ns, weighted_fix_dis_imbalance_pct2_count);
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][FullOrderBook][factor_size_count]"
        << " avg_size_imbalance_avg_ns=" << avg_ns(avg_size_imbalance_ns, avg_size_imbalance_count)
        << " avg_size_imbalance_level1_avg_ns=" << avg_ns(avg_size_imbalance_level1_ns, avg_size_imbalance_level1_count)
        << " avg_size_imbalance_level5_avg_ns=" << avg_ns(avg_size_imbalance_level5_ns, avg_size_imbalance_level5_count)
        << " order_count_imbalance_avg_ns=" << avg_ns(order_count_imbalance_ns, order_count_imbalance_count)
        << " order_count_imbalance_level1_avg_ns="
        << avg_ns(order_count_imbalance_level1_ns, order_count_imbalance_level1_count)
        << " order_count_imbalance_level5_avg_ns="
        << avg_ns(order_count_imbalance_level5_ns, order_count_imbalance_level5_count);
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][FullOrderBook][factor_life_dist]"
        << " order_life_imbalance_avg_ns=" << avg_ns(order_life_imbalance_ns, order_life_imbalance_count)
        << " order_life_imbalance_level1_avg_ns="
        << avg_ns(order_life_imbalance_level1_ns, order_life_imbalance_level1_count)
        << " order_life_imbalance_level5_avg_ns="
        << avg_ns(order_life_imbalance_level5_ns, order_life_imbalance_level5_count)
        << " max_bid_distance_avg_ns=" << avg_ns(max_bid_distance_ns, max_bid_distance_count)
        << " max_ask_distance_avg_ns=" << avg_ns(max_ask_distance_ns, max_ask_distance_count)
        << " max_vol_distance_imbalance_avg_ns="
        << avg_ns(max_vol_distance_imbalance_ns, max_vol_distance_imbalance_count)
        << " young_orderbook_imbalance_avg_ns="
        << avg_ns(young_orderbook_imbalance_ns, young_orderbook_imbalance_count)
        << " fix_dist_hermes_avg_ns=" << avg_ns(fix_dist_hermes_ns, fix_dist_hermes_count);
    lines.push_back(oss.str());

    return lines;
}

std::vector<std::string> ShSzFullOrderBookLatencyStats::format_legacy_predictor_lines() const {
    std::vector<std::string> lines;
    std::ostringstream oss;

    oss << "[Timing][FullOrderBook][legacy_orderflow]"
        << " sample_count=" << legacy_orderflow_count[SHSZ_LEGACY_OF_POSITIVE_ORDER_FLOW]
        << " " << kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_POSITIVE_ORDER_FLOW]
        << "_avg_ns=" << avg_ns(
            legacy_orderflow_ns[SHSZ_LEGACY_OF_POSITIVE_ORDER_FLOW],
            legacy_orderflow_count[SHSZ_LEGACY_OF_POSITIVE_ORDER_FLOW])
        << " " << kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_NEGATIVE_ORDER_FLOW]
        << "_avg_ns=" << avg_ns(
            legacy_orderflow_ns[SHSZ_LEGACY_OF_NEGATIVE_ORDER_FLOW],
            legacy_orderflow_count[SHSZ_LEGACY_OF_NEGATIVE_ORDER_FLOW])
        << " " << kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_MARKET_ORDER_FLOW]
        << "_avg_ns=" << avg_ns(
            legacy_orderflow_ns[SHSZ_LEGACY_OF_MARKET_ORDER_FLOW],
            legacy_orderflow_count[SHSZ_LEGACY_OF_MARKET_ORDER_FLOW])
        << " " << kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_CANCEL_BUY_FLOW]
        << "_avg_ns=" << avg_ns(
            legacy_orderflow_ns[SHSZ_LEGACY_OF_CANCEL_BUY_FLOW],
            legacy_orderflow_count[SHSZ_LEGACY_OF_CANCEL_BUY_FLOW])
        << " " << kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_CANCEL_SELL_FLOW]
        << "_avg_ns=" << avg_ns(
            legacy_orderflow_ns[SHSZ_LEGACY_OF_CANCEL_SELL_FLOW],
            legacy_orderflow_count[SHSZ_LEGACY_OF_CANCEL_SELL_FLOW])
        << " " << kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_POSITIVE_TRADE_FLOW]
        << "_avg_ns=" << avg_ns(
            legacy_orderflow_ns[SHSZ_LEGACY_OF_POSITIVE_TRADE_FLOW],
            legacy_orderflow_count[SHSZ_LEGACY_OF_POSITIVE_TRADE_FLOW])
        << " " << kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_NEGATIVE_TRADE_FLOW]
        << "_avg_ns=" << avg_ns(
            legacy_orderflow_ns[SHSZ_LEGACY_OF_NEGATIVE_TRADE_FLOW],
            legacy_orderflow_count[SHSZ_LEGACY_OF_NEGATIVE_TRADE_FLOW])
        << " " << kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_POSITIVE_TRADE_NUM]
        << "_avg_ns=" << avg_ns(
            legacy_orderflow_ns[SHSZ_LEGACY_OF_POSITIVE_TRADE_NUM],
            legacy_orderflow_count[SHSZ_LEGACY_OF_POSITIVE_TRADE_NUM])
        << " " << kLegacyOrderflowFeatureNames[SHSZ_LEGACY_OF_NEGATIVE_TRADE_NUM]
        << "_avg_ns=" << avg_ns(
            legacy_orderflow_ns[SHSZ_LEGACY_OF_NEGATIVE_TRADE_NUM],
            legacy_orderflow_count[SHSZ_LEGACY_OF_NEGATIVE_TRADE_NUM]);
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][FullOrderBook][legacy_orderbook_core]"
        << " sample_count=" << legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_CROSS_PRICE_RTN]
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_WEIGHTED_CROSS_PRICE_RTN]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_WEIGHTED_CROSS_PRICE_RTN],
            legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_CROSS_PRICE_RTN])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_SQRT_TRADE_RATIO]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_SQRT_TRADE_RATIO],
            legacy_orderbook_count[SHSZ_LEGACY_OB_SQRT_TRADE_RATIO])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_SPREAD]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_SPREAD],
            legacy_orderbook_count[SHSZ_LEGACY_OB_SPREAD])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_MID_RTN]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_MID_RTN],
            legacy_orderbook_count[SHSZ_LEGACY_OB_MID_RTN])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_TICK_SIZE]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_TICK_SIZE],
            legacy_orderbook_count[SHSZ_LEGACY_OB_TICK_SIZE])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_BID_VOL_CHANGE_RATIO]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_BID_VOL_CHANGE_RATIO],
            legacy_orderbook_count[SHSZ_LEGACY_OB_BID_VOL_CHANGE_RATIO])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_ASK_VOL_CHANGE_RATIO]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_ASK_VOL_CHANGE_RATIO],
            legacy_orderbook_count[SHSZ_LEGACY_OB_ASK_VOL_CHANGE_RATIO]);
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][FullOrderBook][legacy_orderbook_weighted]"
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV1]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV1],
            legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV1])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV2]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV2],
            legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV2])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV3]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV3],
            legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV3])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV4]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV4],
            legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV4])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV5]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV5],
            legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_RTN_LV5])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_PCT_WEIGHTED_ASK]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_PCT_WEIGHTED_ASK],
            legacy_orderbook_count[SHSZ_LEGACY_OB_PCT_WEIGHTED_ASK])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_PCT_WEIGHTED_BID]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_PCT_WEIGHTED_BID],
            legacy_orderbook_count[SHSZ_LEGACY_OB_PCT_WEIGHTED_BID])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_WEIGHTED_ASK_RTN]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_WEIGHTED_ASK_RTN],
            legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_ASK_RTN])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_WEIGHTED_BID_RTN]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_WEIGHTED_BID_RTN],
            legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_BID_RTN]);
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][FullOrderBook][legacy_orderbook_tail]"
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_WEIGHTED_IMABALANCE_LV5]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_WEIGHTED_IMABALANCE_LV5],
            legacy_orderbook_count[SHSZ_LEGACY_OB_WEIGHTED_IMABALANCE_LV5])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_IMBALANCE_LV5]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_IMBALANCE_LV5],
            legacy_orderbook_count[SHSZ_LEGACY_OB_IMBALANCE_LV5])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_PCT_TURNOVER]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_PCT_TURNOVER],
            legacy_orderbook_count[SHSZ_LEGACY_OB_PCT_TURNOVER])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_ASK_PCT_LV1]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_ASK_PCT_LV1],
            legacy_orderbook_count[SHSZ_LEGACY_OB_ASK_PCT_LV1])
        << " " << kLegacyOrderbookFeatureNames[SHSZ_LEGACY_OB_BID_PCT_LV1]
        << "_avg_ns=" << avg_ns(
            legacy_orderbook_ns[SHSZ_LEGACY_OB_BID_PCT_LV1],
            legacy_orderbook_count[SHSZ_LEGACY_OB_BID_PCT_LV1]);
    lines.push_back(oss.str());

    return lines;
}

std::vector<std::string> ShSzFullOrderBookLatencyStats::format_lines() const {
    std::vector<std::string> lines;
    if (!shsz_full_orderbook_latency_enabled()) {
        return lines;
    }
    if (total_event_count() == 0) {
        return lines;
    }

    std::ostringstream oss;
    oss << "[Timing][FullOrderBook][event]"
        << " process_order_avg_ns=" << avg_ns(process_order_ns, process_order_count)
        << " process_order_count=" << process_order_count
        << " process_trade_avg_ns=" << avg_ns(process_trade_ns, process_trade_count)
        << " process_trade_count=" << process_trade_count
        << " manager_order_avg_ns=" << avg_ns(manager_order_ns, manager_order_count)
        << " manager_order_count=" << manager_order_count
        << " manager_trade_avg_ns=" << avg_ns(manager_trade_ns, manager_trade_count)
        << " manager_trade_count=" << manager_trade_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][FullOrderBook][transition]"
        << " transition_avg_ns=" << avg_ns(transition_ns, transition_count)
        << " transition_count=" << transition_count
        << " factor_avg_ns=" << avg_ns(factor_ns, factor_count)
        << " factor_count=" << factor_count
        << " project_snapshot_avg_ns=" << avg_ns(project_snapshot_ns, project_snapshot_count)
        << " project_snapshot_count=" << project_snapshot_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][FullOrderBook][predictor]"
        << " may_predict_avg_ns=" << avg_ns(may_predict_ns, may_predict_count)
        << " may_predict_count=" << may_predict_count
        << " do_predict_avg_ns=" << avg_ns(do_predict_ns, do_predict_count)
        << " do_predict_count=" << do_predict_count
        << " flush_pending_avg_ns=" << avg_ns(flush_pending_ns, flush_pending_count)
        << " flush_pending_count=" << flush_pending_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][FullOrderBook][summary]"
        << " process_order_avg_ns=" << avg_ns(process_order_ns, process_order_count)
        << " process_trade_avg_ns=" << avg_ns(process_trade_ns, process_trade_count)
        << " transition_avg_ns=" << avg_ns(transition_ns, transition_count)
        << " factor_avg_ns=" << avg_ns(factor_ns, factor_count)
        << " project_snapshot_avg_ns=" << avg_ns(project_snapshot_ns, project_snapshot_count)
        << " do_predict_avg_ns=" << avg_ns(do_predict_ns, do_predict_count)
        << " flush_pending_avg_ns=" << avg_ns(flush_pending_ns, flush_pending_count);
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    const std::vector<std::string> factor_lines = format_factor_lines();
    lines.insert(lines.end(), factor_lines.begin(), factor_lines.end());
    const std::vector<std::string> legacy_predictor_lines = format_legacy_predictor_lines();
    lines.insert(lines.end(), legacy_predictor_lines.begin(), legacy_predictor_lines.end());

    lines.push_back(format_key_line());
    return lines;
}

void ShSzFullOrderBookLatencyStats::mark_dumped() {
    dumped = true;
}

ShSzFullOrderBookLatencyStats::~ShSzFullOrderBookLatencyStats() {
    if (dumped) {
        return;
    }
    const std::vector<std::string> lines = format_lines();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::cout << lines[i] << std::endl;
    }
}
