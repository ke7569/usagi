#ifndef SSE_T0_STRATEGY_SESSION_H
#define SSE_T0_STRATEGY_SESSION_H

#include "sse/runtime/sse_stream_processor.h"
#include "common/strategy/StrategySession.h"

#include <map>
#include <memory>

namespace sse_strategy {

class Session {
public:
    Session(const nlohmann::json& legacy_config, short execution_source,
            const std::shared_ptr<StrategyExecution>& execution,
            const std::function<bool()>& healthy);
    void set_ready(bool account, bool risk, bool execution);
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

    std::unique_ptr<strategy_runtime::StrategySession> core_;
    std::map<std::string, std::uint64_t> last_exchange_us_;
    // Single-flight gate: while an order for a code is still working, new
    // signals for that code are suppressed. Enabled by default for SSE; the
    // daily processing config may set "sse_single_flight": false to disable.
    bool single_flight_;
};

}  // namespace sse_strategy
#endif
