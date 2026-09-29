#include "common/execution/ExternalExecutionController.h"
#include "common/contracts/legacy/LFConstants.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace strategy_runtime {

ExternalExecutionController::ExternalExecutionController(const std::shared_ptr<StrategyExecution>& backend,
    short source, const std::string& market, const nlohmann::json& inputs)
    : backend_(backend), source_(source), market_(market) {
    if (!backend || !backend->managed()) throw std::invalid_argument("external execution requires managed OMS");
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        Stock stock;
        if (it.value().count("external_delta")) {
            const auto& value = it.value().at("external_delta");
            if (!value.is_number_integer()) throw std::runtime_error("external_delta must be an integer");
            const auto delta = value.get<double>();
            if (delta < -std::numeric_limits<int>::max() || delta > std::numeric_limits<int>::max())
                throw std::runtime_error("external_delta out of range");
            stock.delta = value.get<int>();
        }
        const long long bottom = it.value().at("static_position").get<long long>();
        const long long previous = bottom - stock.delta;
        const long long actual = previous + it.value().value("last_position", 0LL);
        if (previous < 0 || actual < 0 || actual > std::numeric_limits<int>::max())
            throw std::runtime_error("external_delta inconsistent with startup holdings");
        stock.tradable = it.value().value("tradable", true) && !it.value().value("frozen", false);
        stocks_[it.key().substr(0, 6)] = stock;
    }
}

void ExternalExecutionController::begin_round(const std::string& code, const MSMarketDataField& view) {
    if (active_) throw std::logic_error("nested execution decision");
    Stock& stock = stocks_.at(code);
    code_ = code; view_ = view; active_ = true; saw_t0_ = false; due_ = false; continuous_ = false;
    const double time = view.MarketTime;
    if (!std::isfinite(time) || time < 0 || time >= 240000000.0) return;
    const int hhmm = static_cast<int>(time / 100000);
    continuous_ = (hhmm >= 930 && hhmm < 1130) || (hhmm >= 1300 && hhmm < 1457);
    if (hhmm < 930 || hhmm > 1500 || !stock.delta || !stock.tradable) return;
    oms::Position position;
    if (!backend_->read_position(source_, code, market_, &position)) return;
    if (!stock.recovered) {
        std::vector<oms::OrderView> orders;
        if (!backend_->read_execution_orders(source_, code, market_, &orders))
            throw std::runtime_error("OMS execution recovery unavailable");
        for (const auto& order : orders) {
            if (order.command.intent.external_delta != stock.delta)
                throw std::runtime_error("external_delta changed within the trading day");
            if (order.command.id > static_cast<oms::OrderId>(std::numeric_limits<int>::max()))
                throw std::runtime_error("execution order ID out of range");
            if (!order.terminal) stock.orders.push_back(static_cast<int>(order.command.id));
        }
        stock.recovered = true;
    }
    due_ = stock.last_minute != hhmm || stock.retry;
    stock.last_minute = hhmm;
}

oms::Quantity ExternalExecutionController::pe(const std::string& code) const {
    oms::Position position;
    if (!backend_->read_position(source_, code, market_, &position))
        throw std::runtime_error("OMS execution position unavailable");
    if (position.external_target && position.external_target != stocks_.at(code).delta)
        throw std::runtime_error("external_delta changed within the trading day");
    return -static_cast<oms::Quantity>(stocks_.at(code).delta) + position.external_bought - position.external_sold;
}

int ExternalExecutionController::candidate(char direction, double* price) {
    Stock& stock = stocks_.at(code_);
    if (!due_) return 0;
    bool waiting = false;
    for (auto it = stock.orders.begin(); it != stock.orders.end();) {
        oms::OrderView order;
        if (!backend_->read_order(source_, *it, &order)) throw std::runtime_error("OMS execution order unavailable");
        if (!order.terminal && !order.cancel_requested) backend_->cancel(source_, *it);
        if (!backend_->read_order(source_, *it, &order)) throw std::runtime_error("OMS execution order unavailable");
        if (order.terminal) it = stock.orders.erase(it);
        else { waiting = true; ++it; }
    }
    stock.retry = waiting;
    if (!continuous_) { due_ = false; return 0; }
    const oms::Quantity remaining = pe(code_);
    const char side = remaining < 0 ? LF_CHAR_Buy : LF_CHAR_Sell;
    if (direction != side) { stock.retry = false; due_ = false; return 0; }
    if (waiting) return 0;
    due_ = false;
    if (!remaining || !gate_ || !gate_()) return 0;
    const double* values = view_.ms_market_data.ms_market_data.data();
    const int index = side == LF_CHAR_Buy ? AskPrice1Index : BidPrice1Index;
    const int volume = side == LF_CHAR_Buy ? AskVolume1Index : BidVolume1Index;
    for (int level = 0; level < 3; ++level) {
        const double p = values[index + level], q = values[volume + level];
        if (!std::isfinite(p) || !std::isfinite(q) || p <= 0 || q <= 0 ||
            (level && (side == LF_CHAR_Buy ? p < values[index + level - 1] : p > values[index + level - 1]))) return 0;
    }
    if (values[BidPrice1Index] <= 0 || values[AskPrice1Index] < values[BidPrice1Index]) return 0;
    *price = values[index + 2];
    const oms::Quantity quantity = remaining < 0 ? -remaining : remaining;
    if (quantity > std::numeric_limits<int>::max()) throw std::runtime_error("execution remainder out of range");
    return static_cast<int>(quantity);
}

void ExternalExecutionController::track(int id) {
    if (id > 0) stocks_.at(code_).orders.push_back(id);
}

void ExternalExecutionController::finish_round() {
    if (!active_) return;
    try {
        if (!saw_t0_ && due_) {
            const char side = pe(code_) < 0 ? LF_CHAR_Buy : LF_CHAR_Sell;
            double price = 0;
            const int quantity = candidate(side, &price);
            if (quantity) {
                const int id = backend_->submit_allocated(source_, code_, market_, price, quantity, side,
                    side == LF_CHAR_Buy ? LF_CHAR_Open : LF_CHAR_Close, oms::OrderType::Limit, 0, gate_,
                    "external:" + code_ + ":" + std::to_string(stocks_.at(code_).last_minute),
                    quantity, stocks_.at(code_).delta);
                track(id);
                log(id > 0 ? "INFO" : "ERROR", "external execution " + code_ +
                    " quantity=" + std::to_string(quantity) + " request_id=" + std::to_string(id));
            }
        }
        active_ = false;
    } catch (...) { active_ = false; throw; }
}

bool ExternalExecutionController::owns_request(short source, int id, const std::string& code) const {
    return backend_->owns_request(source, id, code);
}
bool ExternalExecutionController::read_position(short source, const std::string& code,
    const std::string& market, oms::Position* out) const { return backend_->read_position(source, code, market, out); }

bool ExternalExecutionController::read_t0_position(short source, const std::string& code,
    const std::string& market, oms::Position* out) const {
    if (!out || source != source_ || market != market_ || !stocks_.count(code) ||
        !backend_->read_position(source, code, market, out)) return false;
    if (out->external_target && out->external_target != stocks_.at(code).delta) return false;
    const oms::Quantity remaining = -static_cast<oms::Quantity>(stocks_.at(code).delta) +
        out->external_bought - out->external_sold;
    out->total -= remaining;
    out->bought -= out->external_bought; out->sold -= out->external_sold;
    out->working_buy -= out->external_working_buy; out->working_sell -= out->external_working_sell;
    out->sellable = std::max<oms::Quantity>(0, out->sellable - out->external_working_sell);
    return true;
}

bool ExternalExecutionController::read_order(short source, int id, oms::OrderView* out) const {
    if (!out || !backend_->read_order(source, id, out)) return false;
    if (!out->command.intent.external_quantity) return true;
    const oms::Quantity t0 = out->command.intent.quantity - out->command.intent.external_quantity;
    out->filled = std::min(out->filled, t0);
    out->working = std::min(out->working, t0 - out->filled);
    out->canceled = out->canceled ? t0 - out->filled : 0;
    out->rejected = out->rejected ? t0 - out->filled : 0;
    out->command.intent.quantity = t0; out->command.intent.external_quantity = 0;
    out->priced_quantity = out->t0_priced_quantity; out->known_amount = out->t0_known_amount;
    out->known_fees = out->t0_known_fees; out->amount_complete = out->priced_quantity == out->filled;
    if (out->filled == t0) { out->terminal = true; out->state = oms::OrderState::Filled; }
    return true;
}
bool ExternalExecutionController::read_day_fills(short source, const std::string& code,
    const std::string& market, oms::Quantity* q, oms::Money* amount) const {
    return backend_->read_t0_day_fills(source, code, market, q, amount);
}
bool ExternalExecutionController::has_working_order(const std::string& code) const {
    oms::Position p;
    return !backend_->read_position(source_, code, market_, &p) ||
        p.working_buy > p.external_working_buy || p.working_sell > p.external_working_sell;
}
int ExternalExecutionController::submit_limit(short source, const std::string& code, const std::string& market,
    double price, int quantity, char side, char offset) {
    return submit_managed(source, code, market, price, quantity, side, offset, oms::OrderType::Limit,
                          0, std::function<bool()>(), std::string());
}
int ExternalExecutionController::submit_managed(short source, const std::string& code, const std::string& market,
    double price, int quantity, char side, char offset, oms::OrderType type, long long delay,
    const std::function<bool()>& gate, const std::string& signal) {
    if (!active_ || code != code_ || source != source_ || market != market_)
        return backend_->submit_managed(source, code, market, price, quantity, side, offset, type, delay, gate, signal);
    saw_t0_ = true;
    double execution_price = price;
    const int external = candidate(side, &execution_price);
    if (external && quantity <= std::numeric_limits<int>::max() - external) {
        const int id = backend_->submit_allocated(source, code, market, execution_price, quantity + external,
            side, offset, type, delay, gate, signal, external, stocks_.at(code).delta);
        if (id > 0) { track(id); return id; }
        log("ERROR", "combined execution rejected; retrying T0 quantity for " + code);
    }
    return backend_->submit_managed(source, code, market, price, quantity, side, offset, type, delay, gate, signal);
}

}  // namespace strategy_runtime
