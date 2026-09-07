#include "common/execution/OmsStrategyExecution.h"
#include "common/contracts/legacy/LFConstants.h"
#include <limits>
#include <stdexcept>

namespace strategy_runtime {

OmsStrategyExecution::OmsStrategyExecution(const std::shared_ptr<oms::Engine>& engine, const std::string& owner)
    : engine_(engine), owner_(owner), sequence_(0) {
    if (!engine || owner.empty() || owner.size() > 96) throw std::invalid_argument("OMS strategy requires engine and owner");
}
bool OmsStrategyExecution::permits_new_orders() const { return engine_->account().ready; }
void OmsStrategyExecution::signal_context(const std::string& signal_id) {
    if (signal_id.size() > 128) throw std::invalid_argument("strategy signal identity too long");
    std::lock_guard<std::mutex> guard(signal_mutex_);
    signal_id_ = signal_id;
}
long long OmsStrategyExecution::now_ns() const { return engine_->account().now_ns; }
bool OmsStrategyExecution::owns_request(short source, int id, const std::string& instrument) const {
    if (source != engine_->scope().source || id <= 0) return false;
    oms::OrderView order;
    return engine_->order(static_cast<oms::OrderId>(id), &order) && order.owned &&
        order.command.intent.owner == owner_ && order.command.intent.instrument.code == instrument;
}
bool OmsStrategyExecution::read_position(short source, const std::string& code, const std::string& market,
                                        oms::Position* output) const {
    return source == engine_->scope().source && engine_->position(oms::Instrument{market, code}, output);
}
int OmsStrategyExecution::submit_limit(short source, const std::string& code, const std::string& market,
                                      double price, int quantity, char direction, char offset) {
    return submit_managed(source, code, market, price, quantity, direction, offset,
                          oms::OrderType::Limit, 0, std::function<bool()>());
}
int OmsStrategyExecution::submit_managed(short source, const std::string& code, const std::string& market,
        double price, int quantity, char direction, char offset, oms::OrderType type, long long delay,
        const std::function<bool()>& gate) {
    if (source != engine_->scope().source || quantity <= 0 ||
        (direction != LF_CHAR_Buy && direction != LF_CHAR_Sell) ||
        (direction == LF_CHAR_Buy ? offset != LF_CHAR_Open : offset != LF_CHAR_Close)) return -1;
    oms::Intent intent;
    if (!oms::money_from_double(price, &intent.price)) return -1;
    intent.owner = owner_; intent.intent_id = std::to_string(engine_->scope().epoch) + ":" + std::to_string(++sequence_);
    { std::lock_guard<std::mutex> guard(signal_mutex_); intent.signal_id = signal_id_; }
    intent.instrument = oms::Instrument{market, code};
    intent.side = direction == LF_CHAR_Buy ? oms::Side::Buy : oms::Side::Sell;
    intent.quantity = quantity; intent.type = type; intent.cancel_delay_ns = delay;
    const oms::SubmitResult result = engine_->submit(intent, gate);
    if (!result.accepted) {
        const int raw = result.error.raw_code;
        if (raw == 2010 || raw == 2011 || raw == 3005 || raw == 3007 || raw == 3008) return -raw;
        switch (result.error.category) {
            case oms::ErrorCategory::Cash: return -3008;
            case oms::ErrorCategory::Shares: return -3007;
            case oms::ErrorCategory::RateLimited: return -2011;
            case oms::ErrorCategory::Capacity: return -2010;
            default: break;
        }
        return -1;
    }
    if (result.id > static_cast<oms::OrderId>(std::numeric_limits<int>::max()))
        throw std::logic_error("OMS violated strategy request ID range");
    return static_cast<int>(result.id);
}
int OmsStrategyExecution::cancel(short source, int id) {
    if (source != engine_->scope().source || id <= 0) return -1;
    const oms::Error result = engine_->cancel(owner_, static_cast<oms::OrderId>(id));
    return !result.failed() || result.category == oms::ErrorCategory::AlreadyFinal ? id : -1;
}
bool OmsStrategyExecution::schedule_cancel(short source, int id, int delay_ms) {
    return source == engine_->scope().source && id > 0 && delay_ms >= 0 &&
        engine_->schedule_cancel(owner_, static_cast<oms::OrderId>(id), delay_ms * 1000000LL);
}
}  // namespace strategy_runtime
