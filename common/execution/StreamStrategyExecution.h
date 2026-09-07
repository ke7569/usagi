#ifndef T0_STREAM_STRATEGY_EXECUTION_H
#define T0_STREAM_STRATEGY_EXECUTION_H

#include "common/contracts/StrategyExecution.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace strategy_runtime {

// Health is re-read at the execution boundary, including asynchronous writer
// failure. Readiness is instance-local; a new session starts closed.
class ProtectedExecution : public StrategyExecution {
public:
    ProtectedExecution(const std::shared_ptr<StrategyExecution>& backend,
                       const std::function<bool()>& healthy,
                       short source, const std::string& exchange,
                       const std::set<std::string>& instruments);
    ~ProtectedExecution() override { begin_stop(); }
    void set_ready(bool account, bool risk, bool execution);
    void begin_stop();
    bool owns_request(short source, int request_id, const std::string& instrument) const override;
    bool managed() const override { return backend_->managed(); }
    void signal_context(const std::string& signal_id) override { backend_->signal_context(signal_id); }
    bool read_position(short, const std::string&, const std::string&, oms::Position*) const override;
    int submit_managed(short, const std::string&, const std::string&, double, int, char, char,
                       oms::OrderType, long long, const std::function<bool()>&) override;
    bool permits_new_orders() const override;
    long long now_ns() const override;
    int submit_limit(short, const std::string&, const std::string&,
                     double, int, char, char) override;
    int cancel(short source, int request_id) override;
    bool schedule_cancel(short source, int request_id, int delay_ms) override;
    void log(const char* level, const std::string& message) override;
private:
    bool allowed() const;
    std::shared_ptr<StrategyExecution> backend_;
    std::function<bool()> healthy_;
    short source_;
    std::string exchange_;
    std::set<std::string> instruments_;
    struct GateState {
        std::atomic<bool> ready, stopping;
        std::function<bool()> healthy;
        explicit GateState(const std::function<bool()>& callback)
            : ready(false), stopping(false), healthy(callback) {}
        bool allowed() const {
            return !stopping.load() && ready.load() && healthy();
        }
    };
    std::shared_ptr<GateState> gate_;
};


}  // namespace strategy_runtime
#endif
