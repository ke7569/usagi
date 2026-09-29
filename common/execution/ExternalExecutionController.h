#ifndef USAGI_EXTERNAL_EXECUTION_CONTROLLER_H
#define USAGI_EXTERNAL_EXECUTION_CONTROLLER_H

#include "common/contracts/StrategyExecution.h"
#include "common/strategy/common.h"
#include "third_party/nlohmann/json.hpp"
#include <map>
#include <memory>

namespace strategy_runtime {

// Owner-thread decision adapter. OMS remains the sole fill/order ledger.
class ExternalExecutionController : public StrategyExecution {
public:
    ExternalExecutionController(const std::shared_ptr<StrategyExecution>& backend,
        short source, const std::string& market, const nlohmann::json& inputs);
    void set_gate(const std::function<bool()>& gate) { gate_ = gate; }
    void begin_round(const std::string& code, const MSMarketDataField& view);
    void finish_round();
    void abort_round() { active_ = false; }
    oms::Quantity pe(const std::string& code) const;

    bool managed() const override { return true; }
    bool permits_new_orders() const override { return backend_->permits_new_orders(); }
    long long now_ns() const override { return backend_->now_ns(); }
    bool owns_request(short, int, const std::string&) const override;
    bool read_position(short, const std::string&, const std::string&, oms::Position*) const override;
    bool read_t0_position(short, const std::string&, const std::string&, oms::Position*) const override;
    bool read_order(short, int, oms::OrderView*) const override;
    bool read_day_fills(short, const std::string&, const std::string&, oms::Quantity*, oms::Money*) const override;
    bool has_working_order(const std::string&) const override;
    int submit_limit(short, const std::string&, const std::string&, double, int, char, char) override;
    int submit_managed(short, const std::string&, const std::string&, double, int, char, char,
        oms::OrderType, long long, const std::function<bool()>&, const std::string& = std::string()) override;
    int cancel(short source, int id) override { return backend_->cancel(source, id); }
    bool schedule_cancel(short source, int id, int delay) override { return backend_->schedule_cancel(source, id, delay); }
    void log(const char* level, const std::string& message) override { backend_->log(level, message); }

private:
    struct Stock {
        int delta = 0;
        bool tradable = true, recovered = false, retry = false;
        int last_minute = -1;
        std::vector<int> orders;
    };
    int candidate(char direction, double* price);
    void track(int id);
    std::shared_ptr<StrategyExecution> backend_;
    short source_;
    std::string market_;
    std::map<std::string, Stock> stocks_;
    std::function<bool()> gate_;
    std::string code_;
    MSMarketDataField view_ = {MSMarketData()};
    bool active_ = false, due_ = false, saw_t0_ = false, continuous_ = false;
};

}  // namespace strategy_runtime
#endif
