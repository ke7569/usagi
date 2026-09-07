#include "common/execution/StreamStrategyExecution.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace strategy_runtime {

ProtectedExecution::ProtectedExecution(const std::shared_ptr<StrategyExecution>& backend,
        const std::function<bool()>& healthy, short source, const std::string& exchange,
        const std::set<std::string>& instruments)
    : backend_(backend), healthy_(healthy), source_(source), exchange_(exchange),
      instruments_(instruments), gate_(new GateState(healthy)) {
    if (!backend_ || !backend_->managed() || !healthy_ || source <= 0 || instruments.empty())
        throw std::invalid_argument("execution boundary requires backend, health and explicit routing");
}

void ProtectedExecution::set_ready(bool account, bool risk, bool execution) {
    gate_->ready.store(account && risk && execution);
}

void ProtectedExecution::begin_stop() {
    gate_->stopping.store(true);
}

bool ProtectedExecution::allowed() const {
    return gate_->allowed() && backend_->permits_new_orders();
}

bool ProtectedExecution::permits_new_orders() const { return allowed(); }
long long ProtectedExecution::now_ns() const { return backend_->now_ns(); }

bool ProtectedExecution::owns_request(short source, int request_id, const std::string& instrument) const {
    return source == source_ && instruments_.count(instrument) && backend_->owns_request(source, request_id, instrument);
}

int ProtectedExecution::submit_limit(short source, const std::string& instrument,
        const std::string& exchange, double price, int volume, char direction, char offset) {
    return submit_managed(source, instrument, exchange, price, volume, direction, offset,
                          oms::OrderType::Limit, 0, std::function<bool()>());
}

int ProtectedExecution::submit_managed(short source, const std::string& instrument,
        const std::string& exchange, double price, int volume, char direction, char offset,
        oms::OrderType type, long long delay, const std::function<bool()>& caller_gate) {
    if (!allowed() || source != source_ || exchange != exchange_ ||
        !instruments_.count(instrument) || !std::isfinite(price) || price <= 0 || volume <= 0)
        return -1;
    const std::shared_ptr<GateState> state = gate_;
    return backend_->submit_managed(source, instrument, exchange, price, volume, direction, offset,
        type, delay, [state, caller_gate]() { return state->allowed() && (!caller_gate || caller_gate()); });
}

bool ProtectedExecution::read_position(short source, const std::string& instrument,
        const std::string& exchange, oms::Position* output) const {
    return source == source_ && exchange == exchange_ && instruments_.count(instrument) &&
        backend_->read_position(source, instrument, exchange, output);
}

int ProtectedExecution::cancel(short source, int request_id) {
    return source == source_ ? backend_->cancel(source, request_id) : -1;
}

bool ProtectedExecution::schedule_cancel(short source, int request_id, int delay_ms) {
    return source == source_ && delay_ms >= 0 &&
        backend_->schedule_cancel(source, request_id, delay_ms);
}

void ProtectedExecution::log(const char* level, const std::string& message) { backend_->log(level, message); }


}  // namespace strategy_runtime
