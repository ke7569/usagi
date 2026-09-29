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
    virtual bool read_t0_position(short source, const std::string& code, const std::string& market,
                                  oms::Position* output) const {
        return read_position(source, code, market, output);
    }
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
    virtual bool read_t0_day_fills(short source, const std::string& code, const std::string& market,
                                  oms::Quantity* quantity, oms::Money* amount) const {
        return read_day_fills(source, code, market, quantity, amount);
    }
    virtual bool read_execution_orders(short, const std::string&, const std::string&,
                                       std::vector<oms::OrderView>*) const { return false; }
    virtual int submit_allocated(short source, const std::string& code, const std::string& market,
        double price, int quantity, char direction, char offset, oms::OrderType type, long long delay,
        const std::function<bool()>& gate, const std::string& signal_id,
        int external_quantity, int external_delta) {
        (void)external_delta;
        return external_quantity == 0 ? submit_managed(source, code, market, price, quantity,
            direction, offset, type, delay, gate, signal_id) : -1;
    }
};

#endif
