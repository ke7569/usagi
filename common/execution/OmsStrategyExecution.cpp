#include "common/execution/OmsStrategyExecution.h"
#include "common/contracts/legacy/LFConstants.h"
#include <limits>
#include <stdexcept>

namespace strategy_runtime {

OmsStrategyExecution::OmsStrategyExecution(const std::shared_ptr<oms::Engine>& engine, const std::string& owner)
    : engine_(engine), owner_(owner), source_(engine ? engine->scope().source : 0), sequence_(0) {
    if (!engine || owner.empty() || owner.size() > 96) throw std::invalid_argument("OMS strategy requires engine and owner");
}
bool OmsStrategyExecution::permits_new_orders() const { return engine_->ready(); }
long long OmsStrategyExecution::now_ns() const { return engine_->now_ns(); }
bool OmsStrategyExecution::owns_request(short source, int id, const std::string& instrument) const {
    if (source != source_ || id <= 0) return false;
    oms::OrderView order;
    return engine_->order(static_cast<oms::OrderId>(id), &order) && order.owned &&
        order.command.intent.owner == owner_ && order.command.intent.instrument.code == instrument;
}
bool OmsStrategyExecution::read_position(short source, const std::string& code, const std::string& market,
                                        oms::Position* output) const {
    return source == source_ && engine_->position(oms::Instrument{market, code}, output);
}
bool OmsStrategyExecution::read_day_fills(short source,const std::string& code,const std::string& market,oms::Quantity* q,oms::Money* amount)const {
    return source==source_ && engine_->day_fills(oms::Instrument{market,code},q,amount);
}
bool OmsStrategyExecution::read_t0_day_fills(short source, const std::string& code, const std::string& market,
                                           oms::Quantity* q, oms::Money* amount) const {
    return source == source_ && engine_->t0_day_fills(oms::Instrument{market, code}, q, amount);
}
bool OmsStrategyExecution::read_execution_orders(short source, const std::string& code, const std::string& market,
                                                std::vector<oms::OrderView>* out) const {
    if (!out || source != source_) return false;
    *out = engine_->execution_orders(owner_, oms::Instrument{market, code}); return true;
}
bool OmsStrategyExecution::has_working_order(const std::string& instrument) const {
    return engine_->has_working_order(oms::Instrument{"SSE", instrument}) ||
           engine_->has_working_order(oms::Instrument{"SZE", instrument});
}
bool OmsStrategyExecution::read_order(short source, int id, oms::OrderView* out) const {
    return out && source == source_ && id > 0 &&
        engine_->order(static_cast<oms::OrderId>(id), out) && out->owned &&
        out->command.intent.owner == owner_;
}
int OmsStrategyExecution::submit_limit(short source, const std::string& code, const std::string& market,
                                      double price, int quantity, char direction, char offset) {
    return submit_managed(source, code, market, price, quantity, direction, offset,
                          oms::OrderType::Limit, 0, std::function<bool()>());
}
int OmsStrategyExecution::submit_managed(short source, const std::string& code, const std::string& market,
        double price, int quantity, char direction, char offset, oms::OrderType type, long long delay,
        const std::function<bool()>& gate, const std::string& signal_id) {
    return submit_allocated(source, code, market, price, quantity, direction, offset, type, delay,
                            gate, signal_id, 0, 0);
}
int OmsStrategyExecution::submit_allocated(short source, const std::string& code, const std::string& market,
        double price, int quantity, char direction, char offset, oms::OrderType type, long long delay,
        const std::function<bool()>& gate, const std::string& signal_id, int external_quantity, int external_delta) {
    if (signal_id.size() > 128) throw std::invalid_argument("strategy signal identity too long");
    if (source != source_ || quantity <= 0 ||
        (direction != LF_CHAR_Buy && direction != LF_CHAR_Sell) ||
        (direction == LF_CHAR_Buy ? offset != LF_CHAR_Open : offset != LF_CHAR_Close)) return -1;
    oms::Intent intent;
    if (!oms::money_from_double(price, &intent.price)) return -1;
    intent.owner = owner_; intent.intent_id = std::to_string(engine_->scope().epoch) + ":" + std::to_string(++sequence_);
    intent.signal_id = signal_id;
    intent.instrument = oms::Instrument{market, code};
    intent.side = direction == LF_CHAR_Buy ? oms::Side::Buy : oms::Side::Sell;
    intent.quantity = quantity; intent.type = type; intent.cancel_delay_ns = delay;
    intent.external_quantity = external_quantity; intent.external_delta = external_delta;
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
    if (source != source_ || id <= 0) return -1;
    const oms::Error result = engine_->cancel(owner_, static_cast<oms::OrderId>(id));
    return !result.failed() || result.category == oms::ErrorCategory::AlreadyFinal ? id : -1;
}
bool OmsStrategyExecution::schedule_cancel(short source, int id, int delay_ms) {
    return source == source_ && id > 0 && delay_ms >= 0 &&
        engine_->schedule_cancel(owner_, static_cast<oms::OrderId>(id), delay_ms * 1000000LL);
}
}  // namespace strategy_runtime
