#ifndef SSE_T0_STRATEGY_SESSION_H
#define SSE_T0_STRATEGY_SESSION_H

#include "sse/runtime/sse_stream_processor.h"
#include "common/strategy/StrategySession.h"

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
    void on_output(const sse_stream::Output& output);
    bool on_order(const LFRtnOrderField& order, int request_id, short source, long received_ns);
    bool on_trade(const LFRtnTradeField& trade, int request_id, short source, long received_ns);
    const MSMarketDataField* last_view(const std::string& instrument) const;
    std::uint64_t signals() const { return core_->signals(); }
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
    std::map<std::string, std::uint64_t> last_exchange_us_;
    std::vector<sse_stream::Output> pending_batch_outputs_;
    bool batch_end_mode_;
    // Single-flight gate: suppress a new signal while an order for the same
    // instrument is still working. Enabled by default for Shanghai.
    bool single_flight_;
    std::function<bool(const std::string&)> instrument_gate_;
};

}  // namespace sse_strategy
#endif
