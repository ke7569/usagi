#ifndef SSE_T0_STRATEGY_SESSION_H
#define SSE_T0_STRATEGY_SESSION_H

#include "sse/runtime/sse_stream_processor.h"
#include "common/strategy/StrategySession.h"
#include "sse/runtime/v06_strategy.h"

#include <map>
#include <memory>
#include <vector>

namespace sse_strategy {

class Session {
public:
    Session(const nlohmann::json& legacy_config, short execution_source,
            const std::shared_ptr<StrategyExecution>& execution,
            const std::function<bool()>& healthy);
    void set_ready(bool account, bool risk, bool execution);
    void set_instrument_gate(const std::function<bool(const std::string&)>& gate) {
        instrument_gate_ = gate;
    }
    void begin_stop();
    void on_timer(std::uint64_t exchange_time_us) { core_->on_timer(exchange_time_us, instrument_gate_); }
    void enable_live_latency(bool enabled){latency_enabled_=enabled;latency_count_=latency_sum_=latency_max_=latency_over_ms_=0;latency_bins_.fill(0);last_latency_count_=last_latency_sum_=last_latency_over_ms_=interval_latency_max_=0;last_latency_bins_.fill(0);}
    nlohmann::json live_latency()const;

    void on_output(const sse_stream::Output& output);
    // Already closed and ordered by the dispatcher; no second Output buffer.
    void on_closed_prediction(const sse_stream::Output& output);
    void on_completed_batch(const std::vector<sse_stream::Output>& outputs);
    bool on_order(const LFRtnOrderField& order, int request_id, short source, long received_ns);
    bool on_trade(const LFRtnTradeField& trade, int request_id, short source, long received_ns);
    const MSMarketDataField* last_view(const std::string& instrument) const;
    std::uint64_t signals() const { return v06_ ? v06_->signals() : core_->signals(); }
    std::shared_ptr<strategy_runtime::ProtectedExecution> execution() const {
        return core_->execution();
    }
    strategy_runtime::StrategySession& core() { return *core_; }
    const strategy_runtime::StrategySession& core() const { return *core_; }

private:
    void process_output(const sse_stream::Output& output);
    void process_prediction_output(const sse_stream::Output& output);
    void flush_batch_outputs();

    std::unique_ptr<strategy_runtime::StrategySession> core_;
    std::unique_ptr<sse_v06::Strategy> v06_;
    MSMarketDataField signal_view_ = {MSMarketData()};
    std::map<std::string, std::uint64_t> last_exchange_us_;
    std::vector<sse_stream::Output> pending_batch_outputs_;
    std::vector<sse_stream::Output> processing_batch_outputs_;
    std::size_t pending_batch_size_ = 0;
    bool batch_end_mode_;
    bool latency_enabled_=false;
    std::uint64_t latency_count_=0,latency_sum_=0,latency_max_=0,latency_over_ms_=0;
    std::array<std::uint64_t,10001> latency_bins_{};
    // Owner-thread status snapshots: interval statistics exclude earlier windows.
    mutable std::uint64_t last_latency_count_=0,last_latency_sum_=0,last_latency_over_ms_=0,interval_latency_max_=0;
    mutable std::array<std::uint64_t,10001> last_latency_bins_{};

    // Single-flight gate: suppress a new signal while an order for the same
    // instrument is still working. Enabled by default for Shanghai.
    bool single_flight_;
    std::function<bool(const std::string&)> instrument_gate_;
};

}  // namespace sse_strategy
#endif
