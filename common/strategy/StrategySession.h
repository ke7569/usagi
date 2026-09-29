#ifndef T0_STRATEGY_SESSION_H
#define T0_STRATEGY_SESSION_H

#include "common/contracts/InsParams.h"
#include "common/execution/StreamStrategyExecution.h"
#include "common/execution/ExternalExecutionController.h"
#include "common/strategy/ZStrategy.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace strategy_runtime {

class StrategySession {
public:
    StrategySession(const nlohmann::json& legacy_config,
                    const std::string& market,
                    short execution_source,
                    const std::shared_ptr<StrategyExecution>& execution,
                    const std::function<bool()>& healthy);

    void set_ready(bool account, bool risk, bool execution);
    void begin_stop();
    void on_signal(const std::string& code, const MSMarketDataField& view,
                   double prediction, long received_time);
    void on_decision(const std::string& code, const MSMarketDataField& view,
                     const std::function<void()>& t0);
    bool has_external_execution() const { return static_cast<bool>(external_); }
    void on_timer(std::uint64_t exchange_time_us,
                  const std::function<bool(const std::string&)>& instrument_gate = {});
    bool on_order(const LFRtnOrderField& order, int request_id,
                  short source, long received_ns);
    bool on_trade(const LFRtnTradeField& trade, int request_id,
                  short source, long received_ns);
    const MSMarketDataField* last_view(const std::string& instrument) const;
    std::uint64_t signals() const { return signals_; }
    std::shared_ptr<ProtectedExecution> execution() const { return execution_; }
    void sync_startup_positions(
        const std::map<std::string, std::pair<int, int> >& total_available);

private:
    struct Instrument {
        MSMarketDataField view;
        std::unique_ptr<ZStrategy> strategy;
        bool have_view;
        Instrument();
    };
    typedef std::map<std::string, std::unique_ptr<Instrument> > Instruments;

    static bool valid_code(const std::string& code, const std::string& market);
    static std::string report_symbol(const char* value, std::size_t length);

    Instruments instruments_;
    std::shared_ptr<ProtectedExecution> execution_;
    std::shared_ptr<ExternalExecutionController> external_;
    short source_;
    std::string market_;
    std::uint64_t signals_;
    std::uint64_t last_timer_second_ = 86400;
};

}  // namespace strategy_runtime

#endif
