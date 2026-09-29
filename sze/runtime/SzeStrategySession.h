#ifndef T0_SZE_STRATEGY_SESSION_H
#define T0_SZE_STRATEGY_SESSION_H

#include "common/strategy/StrategySession.h"
#include "sze/runtime/sze_stream_processor.h"

#include <map>
#include <memory>
#include <string>

namespace sze_strategy {

class Session {
public:
    Session(const nlohmann::json& legacy_config, short execution_source,
            const std::shared_ptr<StrategyExecution>& execution,
            const std::function<bool()>& healthy);

    void set_ready(bool account, bool risk, bool execution);
    void begin_stop();
    void on_timer(std::uint64_t exchange_time_us) { core_.on_timer(exchange_time_us); }
    void on_output(const sze_stream::ProcessedSample& output);
    bool on_order(const LFRtnOrderField& order, int request_id,
                  short source, long received_ns);
    bool on_trade(const LFRtnTradeField& trade, int request_id,
                  short source, long received_ns);
    const MSMarketDataField* last_view(const std::string& instrument) const;
    std::uint64_t signals() const;
    std::shared_ptr<strategy_runtime::ProtectedExecution> execution() const;
    void sync_startup_positions(
        const std::map<std::string, std::pair<int, int> >& total_available);

    strategy_runtime::StrategySession& core() { return core_; }
    const strategy_runtime::StrategySession& core() const { return core_; }

private:
    static std::string normalize_code(const std::string& instrument);
    static double market_time(std::int64_t exchange_time_us);
    static bool valid_sample_view(const mix153060::Sample& sample);
    static MSMarketDataField make_view(const std::string& code,
                                       const mix153060::Sample& sample);

    strategy_runtime::StrategySession core_;
};

}  // namespace sze_strategy

#endif  // T0_SZE_STRATEGY_SESSION_H
