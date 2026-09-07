#include "adapters/deepwin/legacy/bse_latency.h"

#include <chrono>
#include <iostream>
#include <sstream>

#ifdef T0_USE_DEEPWIN
namespace {
kungfu::yijinjing::KfLogPtr g_bse_latency_logger;
}
#endif

void set_bse_latency_logger(const KfLogPtr& logger) {
#ifdef T0_USE_DEEPWIN
    g_bse_latency_logger = logger;
#else
    (void)logger;
#endif
}

BseLatencyStats g_bse_latency_stats;
BseLatencySummaryRecorder g_bse_latency_summary_recorder;

uint64_t bse_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {
uint64_t avg_ns(uint64_t total, uint64_t count) {
    return count == 0 ? 0 : (total / count);
}
}

void BseLatencySummaryRecorder::AtomicMetric::observe(uint64_t latency_ns) {
    total_ns.fetch_add(latency_ns, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    uint64_t current_max = max_ns.load(std::memory_order_relaxed);
    while (current_max < latency_ns &&
           !max_ns.compare_exchange_weak(current_max, latency_ns, std::memory_order_relaxed)) {
    }
}

BseLatencyMetricSnapshot BseLatencySummaryRecorder::AtomicMetric::snapshot_and_reset() {
    BseLatencyMetricSnapshot snapshot;
    snapshot.total_ns = total_ns.exchange(0, std::memory_order_relaxed);
    snapshot.max_ns = max_ns.exchange(0, std::memory_order_relaxed);
    snapshot.count = count.exchange(0, std::memory_order_relaxed);
    return snapshot;
}

void BseLatencySummaryRecorder::observe_tick_total(uint64_t latency_ns) {
    tick_total_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_dispatch_to_worker(uint64_t latency_ns) {
    dispatch_to_worker_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_dispatch_attempt_trace(uint64_t latency_ns) {
    dispatch_attempt_trace_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_dispatch_enqueue(uint64_t latency_ns) {
    dispatch_enqueue_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_dispatch_queued_trace(uint64_t latency_ns) {
    dispatch_queued_trace_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_predictor_do(uint64_t latency_ns) {
    predictor_do_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_on_signal(uint64_t latency_ns) {
    on_signal_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_worker_start_to_intent_built(uint64_t latency_ns) {
    worker_start_to_intent_built_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_intent_to_submit_dequeued(uint64_t latency_ns) {
    intent_to_submit_dequeued_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_submit_to_td_send(uint64_t latency_ns) {
    submit_to_td_send_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_rtn_to_owner_applied(uint64_t latency_ns) {
    rtn_to_owner_applied_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_cancel_total(uint64_t latency_ns) {
    cancel_total_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_submit_total(bool is_buy_side, uint64_t latency_ns) {
    if (is_buy_side) {
        buy_submit_total_.observe(latency_ns);
    } else {
        sell_submit_total_.observe(latency_ns);
    }
}

void BseLatencySummaryRecorder::observe_submit_util(bool is_buy_side, uint64_t latency_ns) {
    if (is_buy_side) {
        buy_submit_util_.observe(latency_ns);
    } else {
        sell_submit_util_.observe(latency_ns);
    }
}

void BseLatencySummaryRecorder::observe_submit_post(bool is_buy_side, uint64_t latency_ns) {
    if (is_buy_side) {
        buy_submit_post_.observe(latency_ns);
    } else {
        sell_submit_post_.observe(latency_ns);
    }
}

void BseLatencySummaryRecorder::observe_tick_to_order_trigger(bool is_buy_side, uint64_t latency_ns) {
    if (is_buy_side) {
        buy_tick_to_order_trigger_.observe(latency_ns);
    } else {
        sell_tick_to_order_trigger_.observe(latency_ns);
    }
}

void BseLatencySummaryRecorder::observe_state_lock_wait(uint64_t latency_ns) {
    state_lock_wait_.observe(latency_ns);
}

void BseLatencySummaryRecorder::observe_portfolio_lock_wait(uint64_t latency_ns) {
    portfolio_lock_wait_.observe(latency_ns);
}

BseLatencySummarySnapshot BseLatencySummaryRecorder::snapshot_and_reset() {
    BseLatencySummarySnapshot snapshot;
    snapshot.tick_total = tick_total_.snapshot_and_reset();
    snapshot.dispatch_to_worker = dispatch_to_worker_.snapshot_and_reset();
    snapshot.dispatch_attempt_trace = dispatch_attempt_trace_.snapshot_and_reset();
    snapshot.dispatch_enqueue = dispatch_enqueue_.snapshot_and_reset();
    snapshot.dispatch_queued_trace = dispatch_queued_trace_.snapshot_and_reset();
    snapshot.predictor_do = predictor_do_.snapshot_and_reset();
    snapshot.on_signal = on_signal_.snapshot_and_reset();
    snapshot.worker_start_to_intent_built = worker_start_to_intent_built_.snapshot_and_reset();
    snapshot.intent_to_submit_dequeued = intent_to_submit_dequeued_.snapshot_and_reset();
    snapshot.submit_to_td_send = submit_to_td_send_.snapshot_and_reset();
    snapshot.rtn_to_owner_applied = rtn_to_owner_applied_.snapshot_and_reset();
    snapshot.cancel_total = cancel_total_.snapshot_and_reset();
    snapshot.buy_submit_total = buy_submit_total_.snapshot_and_reset();
    snapshot.sell_submit_total = sell_submit_total_.snapshot_and_reset();
    snapshot.buy_submit_util = buy_submit_util_.snapshot_and_reset();
    snapshot.sell_submit_util = sell_submit_util_.snapshot_and_reset();
    snapshot.buy_submit_post = buy_submit_post_.snapshot_and_reset();
    snapshot.sell_submit_post = sell_submit_post_.snapshot_and_reset();
    snapshot.buy_tick_to_order_trigger = buy_tick_to_order_trigger_.snapshot_and_reset();
    snapshot.sell_tick_to_order_trigger = sell_tick_to_order_trigger_.snapshot_and_reset();
    snapshot.state_lock_wait = state_lock_wait_.snapshot_and_reset();
    snapshot.portfolio_lock_wait = portfolio_lock_wait_.snapshot_and_reset();
    return snapshot;
}

bool BseLatencySummarySnapshot::empty() const {
    return tick_total.count == 0 &&
        dispatch_to_worker.count == 0 &&
        dispatch_attempt_trace.count == 0 &&
        dispatch_enqueue.count == 0 &&
        dispatch_queued_trace.count == 0 &&
        predictor_do.count == 0 &&
        on_signal.count == 0 &&
        worker_start_to_intent_built.count == 0 &&
        intent_to_submit_dequeued.count == 0 &&
        submit_to_td_send.count == 0 &&
        rtn_to_owner_applied.count == 0 &&
        cancel_total.count == 0 &&
        buy_submit_total.count == 0 &&
        sell_submit_total.count == 0 &&
        buy_submit_util.count == 0 &&
        sell_submit_util.count == 0 &&
        buy_submit_post.count == 0 &&
        sell_submit_post.count == 0 &&
        buy_tick_to_order_trigger.count == 0 &&
        sell_tick_to_order_trigger.count == 0 &&
        state_lock_wait.count == 0 &&
        portfolio_lock_wait.count == 0;
}

std::string BseLatencySummarySnapshot::format_summary_line(const char* runtime_mode, int interval_sec) const {
    std::ostringstream oss;
    oss << "[Timing][BSE][summary60s]"
        << " mode=" << (runtime_mode != nullptr ? runtime_mode : "unknown")
        << " interval_sec=" << interval_sec
        << " manual_rollback_only=1"
        << " tick_avg_ns=" << avg_ns(tick_total.total_ns, tick_total.count)
        << " tick_max_ns=" << tick_total.max_ns
        << " tick_count=" << tick_total.count
        << " dispatch_to_worker_avg_ns=" << avg_ns(dispatch_to_worker.total_ns, dispatch_to_worker.count)
        << " dispatch_to_worker_max_ns=" << dispatch_to_worker.max_ns
        << " dispatch_to_worker_count=" << dispatch_to_worker.count
        << " dispatch_attempt_trace_avg_ns=" << avg_ns(dispatch_attempt_trace.total_ns, dispatch_attempt_trace.count)
        << " dispatch_attempt_trace_max_ns=" << dispatch_attempt_trace.max_ns
        << " dispatch_attempt_trace_count=" << dispatch_attempt_trace.count
        << " dispatch_enqueue_avg_ns=" << avg_ns(dispatch_enqueue.total_ns, dispatch_enqueue.count)
        << " dispatch_enqueue_max_ns=" << dispatch_enqueue.max_ns
        << " dispatch_enqueue_count=" << dispatch_enqueue.count
        << " dispatch_queued_trace_avg_ns=" << avg_ns(dispatch_queued_trace.total_ns, dispatch_queued_trace.count)
        << " dispatch_queued_trace_max_ns=" << dispatch_queued_trace.max_ns
        << " dispatch_queued_trace_count=" << dispatch_queued_trace.count
        << " predictor_do_avg_ns=" << avg_ns(predictor_do.total_ns, predictor_do.count)
        << " predictor_do_max_ns=" << predictor_do.max_ns
        << " predictor_do_count=" << predictor_do.count
        << " on_signal_avg_ns=" << avg_ns(on_signal.total_ns, on_signal.count)
        << " on_signal_max_ns=" << on_signal.max_ns
        << " on_signal_count=" << on_signal.count
        << " worker_start_to_intent_built_avg_ns=" << avg_ns(worker_start_to_intent_built.total_ns, worker_start_to_intent_built.count)
        << " worker_start_to_intent_built_max_ns=" << worker_start_to_intent_built.max_ns
        << " worker_start_to_intent_built_count=" << worker_start_to_intent_built.count
        << " intent_to_submit_dequeued_avg_ns=" << avg_ns(intent_to_submit_dequeued.total_ns, intent_to_submit_dequeued.count)
        << " intent_to_submit_dequeued_max_ns=" << intent_to_submit_dequeued.max_ns
        << " intent_to_submit_dequeued_count=" << intent_to_submit_dequeued.count
        << " submit_to_td_send_avg_ns=" << avg_ns(submit_to_td_send.total_ns, submit_to_td_send.count)
        << " submit_to_td_send_max_ns=" << submit_to_td_send.max_ns
        << " submit_to_td_send_count=" << submit_to_td_send.count
        << " rtn_to_owner_applied_avg_ns=" << avg_ns(rtn_to_owner_applied.total_ns, rtn_to_owner_applied.count)
        << " rtn_to_owner_applied_max_ns=" << rtn_to_owner_applied.max_ns
        << " rtn_to_owner_applied_count=" << rtn_to_owner_applied.count
        << " cancel_avg_ns=" << avg_ns(cancel_total.total_ns, cancel_total.count)
        << " cancel_max_ns=" << cancel_total.max_ns
        << " cancel_count=" << cancel_total.count
        << " buy_submit_avg_ns=" << avg_ns(buy_submit_total.total_ns, buy_submit_total.count)
        << " buy_submit_max_ns=" << buy_submit_total.max_ns
        << " buy_submit_count=" << buy_submit_total.count
        << " sell_submit_avg_ns=" << avg_ns(sell_submit_total.total_ns, sell_submit_total.count)
        << " sell_submit_max_ns=" << sell_submit_total.max_ns
        << " sell_submit_count=" << sell_submit_total.count
        << " buy_submit_util_avg_ns=" << avg_ns(buy_submit_util.total_ns, buy_submit_util.count)
        << " buy_submit_util_max_ns=" << buy_submit_util.max_ns
        << " buy_submit_util_count=" << buy_submit_util.count
        << " sell_submit_util_avg_ns=" << avg_ns(sell_submit_util.total_ns, sell_submit_util.count)
        << " sell_submit_util_max_ns=" << sell_submit_util.max_ns
        << " sell_submit_util_count=" << sell_submit_util.count
        << " buy_submit_post_avg_ns=" << avg_ns(buy_submit_post.total_ns, buy_submit_post.count)
        << " buy_submit_post_max_ns=" << buy_submit_post.max_ns
        << " buy_submit_post_count=" << buy_submit_post.count
        << " sell_submit_post_avg_ns=" << avg_ns(sell_submit_post.total_ns, sell_submit_post.count)
        << " sell_submit_post_max_ns=" << sell_submit_post.max_ns
        << " sell_submit_post_count=" << sell_submit_post.count
        << " buy_trigger_avg_ns=" << avg_ns(buy_tick_to_order_trigger.total_ns, buy_tick_to_order_trigger.count)
        << " buy_trigger_max_ns=" << buy_tick_to_order_trigger.max_ns
        << " buy_trigger_count=" << buy_tick_to_order_trigger.count
        << " sell_trigger_avg_ns=" << avg_ns(sell_tick_to_order_trigger.total_ns, sell_tick_to_order_trigger.count)
        << " sell_trigger_max_ns=" << sell_tick_to_order_trigger.max_ns
        << " sell_trigger_count=" << sell_tick_to_order_trigger.count
        << " state_lock_wait_avg_ns=" << avg_ns(state_lock_wait.total_ns, state_lock_wait.count)
        << " state_lock_wait_max_ns=" << state_lock_wait.max_ns
        << " state_lock_wait_count=" << state_lock_wait.count
        << " portfolio_lock_wait_avg_ns=" << avg_ns(portfolio_lock_wait.total_ns, portfolio_lock_wait.count)
        << " portfolio_lock_wait_max_ns=" << portfolio_lock_wait.max_ns
        << " portfolio_lock_wait_count=" << portfolio_lock_wait.count;
    return oss.str();
}

std::vector<std::string> BseLatencyStats::format_lines() const {
    std::vector<std::string> lines;
    if (tick_count == 0) {
        return lines;
    }
    lines.reserve(9);
    std::ostringstream oss;
    oss << "[Timing][BSE][tick] count=" << tick_count
        << " total_avg_ns=" << avg_ns(tick_total_ns, tick_count)
        << " on_ms_avg_ns=" << avg_ns(on_ms_ns, tick_count);
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][predictor] may_avg_ns=" << avg_ns(may_predict_ns, may_predict_count)
        << " do_avg_ns=" << avg_ns(do_predict_ns, do_predict_count)
        << " ob_factor_avg_ns=" << avg_ns(ob_factor_ns, do_predict_count)
        << " extract_avg_ns=" << avg_ns(extract_features_ns, do_predict_count)
        << " model_avg_ns=" << avg_ns(model_predict_ns, do_predict_count)
        << " do_count=" << do_predict_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][strategy] lookup_avg_ns=" << avg_ns(lookup_ns, lookup_count)
        << " on_signal_avg_ns=" << avg_ns(on_signal_ns, on_signal_count)
        << " copy_avg_ns=" << avg_ns(signal_copy_ns, on_signal_count)
        << " calc_theo_avg_ns=" << avg_ns(calc_theo_ns, calc_theo_count)
        << " hit_buy_avg_ns=" << avg_ns(hit_buy_ns, hit_buy_count)
        << " hit_sell_avg_ns=" << avg_ns(hit_sell_ns, hit_sell_count)
        << " adj_global_skew_avg_ns=" << avg_ns(adj_global_skew_ns, adj_global_skew_count);
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][onsig] pre_handle_avg_ns=" << avg_ns(pre_handle_ns, pre_handle_count)
        << " pre_handle_count=" << pre_handle_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][gate] buy_pass=" << buy_gate_pass_count
        << " buy_block_side=" << buy_gate_block_side_count
        << " buy_block_time=" << buy_gate_block_time_count
        << " sell_pass=" << sell_gate_pass_count
        << " sell_block_side=" << sell_gate_block_side_count
        << " sell_block_time=" << sell_gate_block_time_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][hitpath] buy_calls=" << hit_buy_count
        << " buy_skip_margin=" << hit_buy_skip_margin_count
        << " buy_skip_qty=" << hit_buy_skip_qty_count
        << " buy_order_try=" << hit_buy_order_try_count
        << " buy_order_ok=" << hit_buy_order_ok_count
        << " sell_calls=" << hit_sell_count
        << " sell_skip_margin=" << hit_sell_skip_margin_count
        << " sell_skip_qty=" << hit_sell_skip_qty_count
        << " sell_order_try=" << hit_sell_order_try_count
        << " sell_order_ok=" << hit_sell_order_ok_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][hitphase]"
        << " buy_margin_avg_ns=" << avg_ns(hit_buy_phase_margin_ns, hit_buy_phase_margin_count)
        << " buy_margin_count=" << hit_buy_phase_margin_count
        << " buy_qty_avg_ns=" << avg_ns(hit_buy_phase_qty_ns, hit_buy_phase_qty_count)
        << " buy_qty_count=" << hit_buy_phase_qty_count
        << " buy_insert_avg_ns=" << avg_ns(hit_buy_phase_insert_ns, hit_buy_phase_insert_count)
        << " buy_insert_count=" << hit_buy_phase_insert_count
        << " buy_emit_avg_ns=" << avg_ns(hit_buy_phase_emit_ns, hit_buy_phase_emit_count)
        << " buy_emit_count=" << hit_buy_phase_emit_count
        << " sell_margin_avg_ns=" << avg_ns(hit_sell_phase_margin_ns, hit_sell_phase_margin_count)
        << " sell_margin_count=" << hit_sell_phase_margin_count
        << " sell_qty_avg_ns=" << avg_ns(hit_sell_phase_qty_ns, hit_sell_phase_qty_count)
        << " sell_qty_count=" << hit_sell_phase_qty_count
        << " sell_insert_avg_ns=" << avg_ns(hit_sell_phase_insert_ns, hit_sell_phase_insert_count)
        << " sell_insert_count=" << hit_sell_phase_insert_count
        << " sell_emit_avg_ns=" << avg_ns(hit_sell_phase_emit_ns, hit_sell_phase_emit_count)
        << " sell_emit_count=" << hit_sell_phase_emit_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][insert] buy_avg_ns=" << avg_ns(insert_buy_total_ns, insert_buy_count)
        << " buy_util_avg_ns=" << avg_ns(insert_buy_util_ns, insert_buy_count)
        << " buy_post_avg_ns=" << avg_ns(insert_buy_post_ns, insert_buy_count)
        << " buy_count=" << insert_buy_count
        << " buy_reject=" << insert_buy_reject_count
        << " sell_avg_ns=" << avg_ns(insert_sell_total_ns, insert_sell_count)
        << " sell_util_avg_ns=" << avg_ns(insert_sell_util_ns, insert_sell_count)
        << " sell_post_avg_ns=" << avg_ns(insert_sell_post_ns, insert_sell_count)
        << " sell_count=" << insert_sell_count
        << " sell_reject=" << insert_sell_reject_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][insertdetail] buy_pending_avg_ns=" << avg_ns(insert_buy_pending_ns, insert_buy_pending_count)
        << " buy_pending_count=" << insert_buy_pending_count
        << " buy_tick_to_order_avg_ns=" << avg_ns(insert_buy_tick_to_order_ns, insert_buy_tick_to_order_count)
        << " buy_tick_to_order_count=" << insert_buy_tick_to_order_count
        << " buy_reject_path_avg_ns=" << avg_ns(insert_buy_reject_path_ns, insert_buy_reject_path_count)
        << " buy_reject_path_count=" << insert_buy_reject_path_count
        << " sell_pending_avg_ns=" << avg_ns(insert_sell_pending_ns, insert_sell_pending_count)
        << " sell_pending_count=" << insert_sell_pending_count
        << " sell_tick_to_order_avg_ns=" << avg_ns(insert_sell_tick_to_order_ns, insert_sell_tick_to_order_count)
        << " sell_tick_to_order_count=" << insert_sell_tick_to_order_count
        << " sell_reject_path_avg_ns=" << avg_ns(insert_sell_reject_path_ns, insert_sell_reject_path_count)
        << " sell_reject_path_count=" << insert_sell_reject_path_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][predemit] total_avg_ns=" << avg_ns(predict_emit_total_ns, predict_emit_total_count)
        << " total_count=" << predict_emit_total_count
        << " format_avg_ns=" << avg_ns(predict_emit_format_ns, predict_emit_format_count)
        << " format_count=" << predict_emit_format_count
        << " sync_avg_ns=" << avg_ns(predict_emit_sync_ns, predict_emit_sync_count)
        << " sync_count=" << predict_emit_sync_count
        << " fallback=" << predict_emit_fallback_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][BSE][lock] state_wait_avg_ns=" << avg_ns(state_lock_wait_ns, state_lock_wait_count)
        << " state_wait_max_ns=" << state_lock_wait_max_ns
        << " state_wait_count=" << state_lock_wait_count
        << " portfolio_wait_avg_ns=" << avg_ns(portfolio_lock_wait_ns, portfolio_lock_wait_count)
        << " portfolio_wait_max_ns=" << portfolio_lock_wait_max_ns
        << " portfolio_wait_count=" << portfolio_lock_wait_count;
    lines.push_back(oss.str());
    return lines;
}

std::string BseLatencyStats::format_summary_line(const char* runtime_mode, int interval_sec) const {
    std::ostringstream oss;
    oss << "[Timing][BSE][summary]"
        << " mode=" << (runtime_mode != nullptr ? runtime_mode : "unknown")
        << " interval_sec=" << interval_sec
        << " tick_count=" << tick_count
        << " tick_avg_ns=" << avg_ns(tick_total_ns, tick_count)
        << " predictor_do_avg_ns=" << avg_ns(do_predict_ns, do_predict_count)
        << " on_signal_avg_ns=" << avg_ns(on_signal_ns, on_signal_count)
        << " buy_insert_avg_ns=" << avg_ns(insert_buy_total_ns, insert_buy_count)
        << " sell_insert_avg_ns=" << avg_ns(insert_sell_total_ns, insert_sell_count)
        << " buy_tick_to_order_avg_ns=" << avg_ns(insert_buy_tick_to_order_ns, insert_buy_tick_to_order_count)
        << " sell_tick_to_order_avg_ns=" << avg_ns(insert_sell_tick_to_order_ns, insert_sell_tick_to_order_count)
        << " buy_insert_count=" << insert_buy_count
        << " sell_insert_count=" << insert_sell_count;
    return oss.str();
}

void BseLatencyStats::mark_dumped() {
    dumped = true;
}

BseLatencyStats::~BseLatencyStats() {
    if (dumped) {
        return;
    }
    const auto lines = format_lines();
    if (lines.empty()) {
        return;
    }
#ifdef T0_USE_DEEPWIN
    if (g_bse_latency_logger) {
        for (const auto& line : lines) {
            KF_LOG_INFO(g_bse_latency_logger, line);
        }
        return;
    }
#endif
    for (const auto& line : lines) {
        std::cout << line << std::endl;
    }
}
