#ifndef BSE_LATENCY_H
#define BSE_LATENCY_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "adapters/deepwin/legacy/util.h"

void set_bse_latency_logger(const KfLogPtr& logger);

struct BseLatencyStats {
    uint64_t tick_count = 0;
    uint64_t tick_total_ns = 0;

    uint64_t on_ms_ns = 0;
    uint64_t lookup_ns = 0;
    uint64_t may_predict_ns = 0;
    uint64_t do_predict_ns = 0;
    uint64_t ob_factor_ns = 0;
    uint64_t extract_features_ns = 0;
    uint64_t model_predict_ns = 0;

    uint64_t on_signal_ns = 0;
    uint64_t signal_copy_ns = 0;
    uint64_t calc_theo_ns = 0;
    uint64_t hit_buy_ns = 0;
    uint64_t hit_sell_ns = 0;
    uint64_t adj_global_skew_ns = 0;
    uint64_t pre_handle_ns = 0;

    uint64_t lookup_count = 0;
    uint64_t may_predict_count = 0;
    uint64_t do_predict_count = 0;
    uint64_t on_signal_count = 0;
    uint64_t calc_theo_count = 0;
    uint64_t hit_buy_count = 0;
    uint64_t hit_sell_count = 0;
    uint64_t adj_global_skew_count = 0;
    uint64_t pre_handle_count = 0;

    uint64_t buy_gate_pass_count = 0;
    uint64_t buy_gate_block_side_count = 0;
    uint64_t buy_gate_block_time_count = 0;
    uint64_t sell_gate_pass_count = 0;
    uint64_t sell_gate_block_side_count = 0;
    uint64_t sell_gate_block_time_count = 0;

    uint64_t hit_buy_skip_margin_count = 0;
    uint64_t hit_buy_skip_qty_count = 0;
    uint64_t hit_buy_order_try_count = 0;
    uint64_t hit_buy_order_ok_count = 0;
    uint64_t hit_sell_skip_margin_count = 0;
    uint64_t hit_sell_skip_qty_count = 0;
    uint64_t hit_sell_order_try_count = 0;
    uint64_t hit_sell_order_ok_count = 0;

    // handle_t0 phase timing (segment cost, not whole-function average)
    // phase order:
    // 1) margin computed
    // 2) order quantity computed
    // 3) insertOrder finished
    // 4) EmitHitLine finished
    uint64_t hit_buy_phase_margin_ns = 0;
    uint64_t hit_buy_phase_margin_count = 0;
    uint64_t hit_buy_phase_qty_ns = 0;
    uint64_t hit_buy_phase_qty_count = 0;
    uint64_t hit_buy_phase_insert_ns = 0;
    uint64_t hit_buy_phase_insert_count = 0;
    uint64_t hit_buy_phase_emit_ns = 0;
    uint64_t hit_buy_phase_emit_count = 0;

    uint64_t hit_sell_phase_margin_ns = 0;
    uint64_t hit_sell_phase_margin_count = 0;
    uint64_t hit_sell_phase_qty_ns = 0;
    uint64_t hit_sell_phase_qty_count = 0;
    uint64_t hit_sell_phase_insert_ns = 0;
    uint64_t hit_sell_phase_insert_count = 0;
    uint64_t hit_sell_phase_emit_ns = 0;
    uint64_t hit_sell_phase_emit_count = 0;

    uint64_t insert_buy_total_ns = 0;
    uint64_t insert_buy_util_ns = 0;
    uint64_t insert_buy_post_ns = 0;
    uint64_t insert_buy_count = 0;
    uint64_t insert_buy_reject_count = 0;
    uint64_t insert_sell_total_ns = 0;
    uint64_t insert_sell_util_ns = 0;
    uint64_t insert_sell_post_ns = 0;
    uint64_t insert_sell_count = 0;
    uint64_t insert_sell_reject_count = 0;

    uint64_t insert_buy_pending_ns = 0;
    uint64_t insert_buy_pending_count = 0;
    uint64_t insert_buy_tick_to_order_ns = 0;
    uint64_t insert_buy_tick_to_order_count = 0;
    uint64_t insert_buy_reject_path_ns = 0;
    uint64_t insert_buy_reject_path_count = 0;
    uint64_t insert_sell_pending_ns = 0;
    uint64_t insert_sell_pending_count = 0;
    uint64_t insert_sell_tick_to_order_ns = 0;
    uint64_t insert_sell_tick_to_order_count = 0;
    uint64_t insert_sell_reject_path_ns = 0;
    uint64_t insert_sell_reject_path_count = 0;

    uint64_t predict_emit_total_ns = 0;
    uint64_t predict_emit_total_count = 0;
    uint64_t predict_emit_format_ns = 0;
    uint64_t predict_emit_format_count = 0;
    uint64_t predict_emit_sync_ns = 0;
    uint64_t predict_emit_sync_count = 0;
    uint64_t predict_emit_fallback_count = 0;
    uint64_t state_lock_wait_ns = 0;
    uint64_t state_lock_wait_count = 0;
    uint64_t state_lock_wait_max_ns = 0;
    uint64_t portfolio_lock_wait_ns = 0;
    uint64_t portfolio_lock_wait_count = 0;
    uint64_t portfolio_lock_wait_max_ns = 0;
    bool dumped = false;

    std::vector<std::string> format_lines() const;
    std::string format_summary_line(const char* runtime_mode, int interval_sec) const;
    void mark_dumped();
    ~BseLatencyStats();
};

struct BseLatencyMetricSnapshot {
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
    uint64_t count = 0;
};

struct BseLatencySummarySnapshot {
    BseLatencyMetricSnapshot tick_total;
    BseLatencyMetricSnapshot dispatch_to_worker;
    BseLatencyMetricSnapshot dispatch_attempt_trace;
    BseLatencyMetricSnapshot dispatch_enqueue;
    BseLatencyMetricSnapshot dispatch_queued_trace;
    BseLatencyMetricSnapshot predictor_do;
    BseLatencyMetricSnapshot on_signal;
    BseLatencyMetricSnapshot worker_start_to_intent_built;
    BseLatencyMetricSnapshot intent_to_submit_dequeued;
    BseLatencyMetricSnapshot submit_to_td_send;
    BseLatencyMetricSnapshot rtn_to_owner_applied;
    BseLatencyMetricSnapshot cancel_total;
    BseLatencyMetricSnapshot buy_submit_total;
    BseLatencyMetricSnapshot sell_submit_total;
    BseLatencyMetricSnapshot buy_submit_util;
    BseLatencyMetricSnapshot sell_submit_util;
    BseLatencyMetricSnapshot buy_submit_post;
    BseLatencyMetricSnapshot sell_submit_post;
    BseLatencyMetricSnapshot buy_tick_to_order_trigger;
    BseLatencyMetricSnapshot sell_tick_to_order_trigger;
    BseLatencyMetricSnapshot state_lock_wait;
    BseLatencyMetricSnapshot portfolio_lock_wait;

    bool empty() const;
    std::string format_summary_line(const char* runtime_mode, int interval_sec) const;
};

class BseLatencySummaryRecorder {
public:
    void observe_tick_total(uint64_t latency_ns);
    void observe_dispatch_to_worker(uint64_t latency_ns);
    void observe_dispatch_attempt_trace(uint64_t latency_ns);
    void observe_dispatch_enqueue(uint64_t latency_ns);
    void observe_dispatch_queued_trace(uint64_t latency_ns);
    void observe_predictor_do(uint64_t latency_ns);
    void observe_on_signal(uint64_t latency_ns);
    void observe_worker_start_to_intent_built(uint64_t latency_ns);
    void observe_intent_to_submit_dequeued(uint64_t latency_ns);
    void observe_submit_to_td_send(uint64_t latency_ns);
    void observe_rtn_to_owner_applied(uint64_t latency_ns);
    void observe_cancel_total(uint64_t latency_ns);
    void observe_submit_total(bool is_buy_side, uint64_t latency_ns);
    void observe_submit_util(bool is_buy_side, uint64_t latency_ns);
    void observe_submit_post(bool is_buy_side, uint64_t latency_ns);
    void observe_tick_to_order_trigger(bool is_buy_side, uint64_t latency_ns);
    void observe_state_lock_wait(uint64_t latency_ns);
    void observe_portfolio_lock_wait(uint64_t latency_ns);

    BseLatencySummarySnapshot snapshot_and_reset();

private:
    struct AtomicMetric {
        std::atomic<uint64_t> total_ns{0};
        std::atomic<uint64_t> max_ns{0};
        std::atomic<uint64_t> count{0};

        void observe(uint64_t latency_ns);
        BseLatencyMetricSnapshot snapshot_and_reset();
    };

    AtomicMetric tick_total_;
    AtomicMetric dispatch_to_worker_;
    AtomicMetric dispatch_attempt_trace_;
    AtomicMetric dispatch_enqueue_;
    AtomicMetric dispatch_queued_trace_;
    AtomicMetric predictor_do_;
    AtomicMetric on_signal_;
    AtomicMetric worker_start_to_intent_built_;
    AtomicMetric intent_to_submit_dequeued_;
    AtomicMetric submit_to_td_send_;
    AtomicMetric rtn_to_owner_applied_;
    AtomicMetric cancel_total_;
    AtomicMetric buy_submit_total_;
    AtomicMetric sell_submit_total_;
    AtomicMetric buy_submit_util_;
    AtomicMetric sell_submit_util_;
    AtomicMetric buy_submit_post_;
    AtomicMetric sell_submit_post_;
    AtomicMetric buy_tick_to_order_trigger_;
    AtomicMetric sell_tick_to_order_trigger_;
    AtomicMetric state_lock_wait_;
    AtomicMetric portfolio_lock_wait_;
};

uint64_t bse_now_ns();

extern BseLatencyStats g_bse_latency_stats;
extern BseLatencySummaryRecorder g_bse_latency_summary_recorder;

#endif // BSE_LATENCY_H
