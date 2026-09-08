#ifndef USAGI_OMS_STRATEGY_EXECUTION_H
#define USAGI_OMS_STRATEGY_EXECUTION_H

#include "common/contracts/StrategyExecution.h"
#include "common/oms/Oms.h"
#include <atomic>

namespace strategy_runtime {

// A client identity, not a second order or position ledger.
class OmsStrategyExecution : public StrategyExecution {
public:
    OmsStrategyExecution(const std::shared_ptr<oms::Engine>& engine, const std::string& owner);
    bool managed() const override { return true; }
    void signal_context(const std::string& signal_id) override;
    bool permits_new_orders() const override;
    long long now_ns() const override;
    bool owns_request(short source, int id, const std::string& instrument) const override;
    bool read_position(short, const std::string&, const std::string&, oms::Position*) const override;
    bool has_working_order(const std::string& instrument) const override;
    int submit_limit(short, const std::string&, const std::string&, double, int, char, char) override;
    int submit_managed(short, const std::string&, const std::string&, double, int, char, char,
                       oms::OrderType, long long, const std::function<bool()>&) override;
    int cancel(short source, int id) override;
    bool schedule_cancel(short source, int id, int delay_ms) override;
    std::shared_ptr<oms::Engine> engine() const { return engine_; }
private:
    std::shared_ptr<oms::Engine> engine_;
    std::string owner_;
    std::atomic<std::uint64_t> sequence_;
    mutable std::mutex signal_mutex_;
    std::string signal_id_;
};

}  // namespace strategy_runtime
#endif
