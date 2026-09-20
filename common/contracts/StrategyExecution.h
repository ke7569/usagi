#ifndef T0_STRATEGY_EXECUTION_H
#define T0_STRATEGY_EXECUTION_H

#include "common/oms/Types.h"
#include <functional>
#include <string>

class StrategyExecution {
public:
    virtual ~StrategyExecution() {}

    virtual bool permits_new_orders() const = 0;
    virtual long long now_ns() const = 0;
    virtual int submit_limit(short source, const std::string& instrument,
                             const std::string& exchange, double price,
                             int volume, char direction, char offset) = 0;
    virtual int cancel(short source, int request_id) = 0;
    virtual bool schedule_cancel(short source, int request_id, int delay_ms) = 0;
    virtual bool managed() const { return false; }
    virtual bool owns_request(short, int, const std::string&) const { return false; }
    virtual bool read_position(short, const std::string&, const std::string&, oms::Position*) const { return false; }
    // Single-flight query: whether an order for the instrument is still working.
    // Defaults to false so legacy/unmanaged executors never gate on it.
    virtual bool has_working_order(const std::string& instrument) const { return false; }
    virtual int submit_managed(short, const std::string&, const std::string&, double, int, char, char,
                               oms::OrderType, long long, const std::function<bool()>&,
                               const std::string& signal_id = std::string()) { return -1; }
    int submit_limit_then_cancel(short source, const std::string& instrument, const std::string& exchange,
                                double price, int volume, char direction, char offset, int delay_ms,
                                const std::string& signal_id = std::string()) {
        return submit_managed(source, instrument, exchange, price, volume, direction, offset,
            oms::OrderType::LimitThenCancel, delay_ms * 1000000LL, std::function<bool()>(), signal_id);
    }
    virtual void log(const char* level, const std::string& message) {
        (void)level;
        (void)message;
    }
    // Internal strategy interface; rebuild its callers and implementations together.
    virtual bool read_order(short, int, oms::OrderView*) const { return false; }
    virtual bool read_day_fills(short, const std::string&, const std::string&, oms::Quantity*, oms::Money*) const { return false; }
};

#endif
