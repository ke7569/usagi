#ifndef SHSZ_FULL_ORDERBOOK_DIAGNOSTICS_H
#define SHSZ_FULL_ORDERBOOK_DIAGNOSTICS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum ShSzLegacyOrderflowFeatureIndex {
    SHSZ_LEGACY_OF_POSITIVE_ORDER_FLOW = 0,
    SHSZ_LEGACY_OF_NEGATIVE_ORDER_FLOW = 1,
    SHSZ_LEGACY_OF_MARKET_ORDER_FLOW = 2,
    SHSZ_LEGACY_OF_CANCEL_BUY_FLOW = 3,
    SHSZ_LEGACY_OF_CANCEL_SELL_FLOW = 4,
    SHSZ_LEGACY_OF_POSITIVE_TRADE_FLOW = 5,
    SHSZ_LEGACY_OF_NEGATIVE_TRADE_FLOW = 6,
    SHSZ_LEGACY_OF_POSITIVE_TRADE_NUM = 7,
    SHSZ_LEGACY_OF_NEGATIVE_TRADE_NUM = 8,
    SHSZ_LEGACY_OF_FEATURE_COUNT = 9
};

enum ShSzLegacyOrderbookFeatureIndex {
    SHSZ_LEGACY_OB_WEIGHTED_CROSS_PRICE_RTN = 0,
    SHSZ_LEGACY_OB_SQRT_TRADE_RATIO = 1,
    SHSZ_LEGACY_OB_SPREAD = 2,
    SHSZ_LEGACY_OB_MID_RTN = 3,
    SHSZ_LEGACY_OB_TICK_SIZE = 4,
    SHSZ_LEGACY_OB_BID_VOL_CHANGE_RATIO = 5,
    SHSZ_LEGACY_OB_ASK_VOL_CHANGE_RATIO = 6,
    SHSZ_LEGACY_OB_WEIGHTED_RTN_LV1 = 7,
    SHSZ_LEGACY_OB_WEIGHTED_RTN_LV2 = 8,
    SHSZ_LEGACY_OB_WEIGHTED_RTN_LV3 = 9,
    SHSZ_LEGACY_OB_WEIGHTED_RTN_LV4 = 10,
    SHSZ_LEGACY_OB_WEIGHTED_RTN_LV5 = 11,
    SHSZ_LEGACY_OB_PCT_WEIGHTED_ASK = 12,
    SHSZ_LEGACY_OB_PCT_WEIGHTED_BID = 13,
    SHSZ_LEGACY_OB_WEIGHTED_ASK_RTN = 14,
    SHSZ_LEGACY_OB_WEIGHTED_BID_RTN = 15,
    SHSZ_LEGACY_OB_WEIGHTED_IMABALANCE_LV5 = 16,
    SHSZ_LEGACY_OB_IMBALANCE_LV5 = 17,
    SHSZ_LEGACY_OB_PCT_TURNOVER = 18,
    SHSZ_LEGACY_OB_ASK_PCT_LV1 = 19,
    SHSZ_LEGACY_OB_BID_PCT_LV1 = 20,
    SHSZ_LEGACY_OB_FEATURE_COUNT = 21
};

struct ShSzFullOrderBookLatencyStats {
    uint32_t last_event_time_ms = 0;

    uint64_t positive_fill_rate_count = 0;
    uint64_t positive_fill_rate_ns = 0;
    uint64_t negative_fill_rate_count = 0;
    uint64_t negative_fill_rate_ns = 0;
    uint64_t order_flow_imbalance_count = 0;
    uint64_t order_flow_imbalance_ns = 0;
    uint64_t cfr_imbalance_count = 0;
    uint64_t cfr_imbalance_ns = 0;
    uint64_t fix_dis_imbalance_pct1_count = 0;
    uint64_t fix_dis_imbalance_pct1_ns = 0;
    uint64_t fix_dis_imbalance_pct2_count = 0;
    uint64_t fix_dis_imbalance_pct2_ns = 0;
    uint64_t weighted_fix_dis_imbalance_pct1_count = 0;
    uint64_t weighted_fix_dis_imbalance_pct1_ns = 0;
    uint64_t weighted_fix_dis_imbalance_pct2_count = 0;
    uint64_t weighted_fix_dis_imbalance_pct2_ns = 0;
    uint64_t avg_size_imbalance_count = 0;
    uint64_t avg_size_imbalance_ns = 0;
    uint64_t avg_size_imbalance_level1_count = 0;
    uint64_t avg_size_imbalance_level1_ns = 0;
    uint64_t avg_size_imbalance_level5_count = 0;
    uint64_t avg_size_imbalance_level5_ns = 0;
    uint64_t order_count_imbalance_count = 0;
    uint64_t order_count_imbalance_ns = 0;
    uint64_t order_count_imbalance_level1_count = 0;
    uint64_t order_count_imbalance_level1_ns = 0;
    uint64_t order_count_imbalance_level5_count = 0;
    uint64_t order_count_imbalance_level5_ns = 0;
    uint64_t order_life_imbalance_count = 0;
    uint64_t order_life_imbalance_ns = 0;
    uint64_t order_life_imbalance_level1_count = 0;
    uint64_t order_life_imbalance_level1_ns = 0;
    uint64_t order_life_imbalance_level5_count = 0;
    uint64_t order_life_imbalance_level5_ns = 0;
    uint64_t max_bid_distance_count = 0;
    uint64_t max_bid_distance_ns = 0;
    uint64_t max_ask_distance_count = 0;
    uint64_t max_ask_distance_ns = 0;
    uint64_t max_vol_distance_imbalance_count = 0;
    uint64_t max_vol_distance_imbalance_ns = 0;
    uint64_t young_orderbook_imbalance_count = 0;
    uint64_t young_orderbook_imbalance_ns = 0;
    uint64_t fix_dist_hermes_count = 0;
    uint64_t fix_dist_hermes_ns = 0;
    std::array<uint64_t, SHSZ_LEGACY_OF_FEATURE_COUNT> legacy_orderflow_count = {};
    std::array<uint64_t, SHSZ_LEGACY_OF_FEATURE_COUNT> legacy_orderflow_ns = {};
    std::array<uint64_t, SHSZ_LEGACY_OB_FEATURE_COUNT> legacy_orderbook_count = {};
    std::array<uint64_t, SHSZ_LEGACY_OB_FEATURE_COUNT> legacy_orderbook_ns = {};

    uint64_t process_order_count = 0;
    uint64_t process_order_ns = 0;
    uint64_t process_trade_count = 0;
    uint64_t process_trade_ns = 0;

    uint64_t manager_order_count = 0;
    uint64_t manager_order_ns = 0;
    uint64_t manager_trade_count = 0;
    uint64_t manager_trade_ns = 0;

    uint64_t transition_count = 0;
    uint64_t transition_ns = 0;
    uint64_t factor_count = 0;
    uint64_t factor_ns = 0;
    uint64_t project_snapshot_count = 0;
    uint64_t project_snapshot_ns = 0;

    uint64_t may_predict_count = 0;
    uint64_t may_predict_ns = 0;
    uint64_t do_predict_count = 0;
    uint64_t do_predict_ns = 0;
    uint64_t flush_pending_count = 0;
    uint64_t flush_pending_ns = 0;

    bool dumped = false;

    uint64_t total_event_count() const;
    void add_legacy_orderflow_sample(size_t index, uint64_t elapsed_ns);
    void add_legacy_orderbook_sample(size_t index, uint64_t elapsed_ns);
    std::string format_key_line() const;
    std::vector<std::string> format_factor_lines() const;
    std::vector<std::string> format_legacy_predictor_lines() const;
    std::vector<std::string> format_lines() const;
    void mark_dumped();
    ~ShSzFullOrderBookLatencyStats();
};

uint64_t shsz_full_orderbook_now_ns();
bool shsz_full_orderbook_latency_enabled();
void shsz_full_orderbook_set_latency_enabled(bool enabled);
ShSzFullOrderBookLatencyStats& shsz_full_orderbook_latency_stats();

#endif // SHSZ_FULL_ORDERBOOK_DIAGNOSTICS_H
