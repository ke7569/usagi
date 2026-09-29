#include "common/strategy/StrategySession.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace strategy_runtime {
namespace {

typedef nlohmann::json Json;

double finite_number(const Json& object, const char* key) {
    const Json& value = object.at(key);
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        throw std::runtime_error(std::string("strategy requires finite ") + key);
    return value.get<double>();
}

int position(const Json& object, const char* key, bool allow_negative) {
    const double value = finite_number(object, key);
    if (value != std::floor(value) ||
        value < (allow_negative ? std::numeric_limits<int>::min() : 0) ||
        value > std::numeric_limits<int>::max())
        throw std::runtime_error(std::string("invalid strategy ") + key);
    return static_cast<int>(value);
}

}  // namespace

StrategySession::Instrument::Instrument()
    : view{MSMarketData()}, strategy(), have_view(false) {}

bool StrategySession::valid_code(const std::string& code,
                                 const std::string& market) {
    if (code.size() != 6U) return false;
    for (std::size_t i = 0; i < code.size(); ++i)
        if (code[i] < '0' || code[i] > '9') return false;
    if (market == "SH") return code.compare(0, 2, "60") == 0 ||
                              code.compare(0, 2, "68") == 0;
    if (market == "SZ") return code.compare(0, 2, "00") == 0 ||
                              code.compare(0, 2, "30") == 0;
    return false;
}

std::string StrategySession::report_symbol(const char* value, std::size_t length) {
    if (value == 0) return std::string();
    const char* end = static_cast<const char*>(std::memchr(value, 0, length));
    return end ? std::string(value, end) : std::string();
}

StrategySession::StrategySession(
    const Json& legacy_config, const std::string& market,
    short execution_source, const std::shared_ptr<StrategyExecution>& execution,
    const std::function<bool()>& healthy)
    : instruments_(), execution_(), source_(execution_source), market_(market),
      signals_(0) {
    if (market_ != "SH" && market_ != "SZ")
        throw std::runtime_error("strategy session requires market SH or SZ");
    if (execution_source <= 0 || !execution || !healthy)
        throw std::invalid_argument("strategy session requires execution and health");
    if (!legacy_config.at("market").is_string() ||
        legacy_config.at("market").get<std::string>() != market_)
        throw std::runtime_error("strategy session market mismatch");

    const Json& globals = legacy_config.at("global_params");
    for (const char* key : {"offset", "position_base_line", "position_limit"})
        if (finite_number(globals, key) <= 0.0)
            throw std::runtime_error("strategy scale must be positive");
    const char* bias_key = globals.count("global_bias_factor")
        ? "global_bias_factor" : "bias_factor";
    if (finite_number(globals, bias_key) <= 0.0)
        throw std::runtime_error("strategy bias must be positive");

    const Json& inputs = legacy_config.at("ins_params");
    const char* test_order = market_ == "SH" ? "sse_test_order" : "sze_test_order";
    if (legacy_config.count(test_order) && legacy_config.at(test_order).value("enabled", false))
        throw std::runtime_error("stream strategy does not enable diagnostic test orders");
    if (!inputs.is_object() || inputs.empty())
        throw std::runtime_error("strategy instruments required");
    std::set<std::string> universe;
    for (Json::const_iterator it = inputs.begin(); it != inputs.end(); ++it) {
        const std::string suffix = market_ == "SH" ? ".SH" : ".SZ";
        const std::string& symbol = it.key();
        if (symbol.size() != 9U || symbol.substr(6) != suffix ||
            !valid_code(symbol.substr(0, 6), market_))
            throw std::runtime_error("strategy requires canonical market stock symbols");
        universe.insert(symbol.substr(0, 6));
    }

    std::shared_ptr<StrategyExecution> decisions = execution;
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        if (it.value().count("external_delta")) {
            external_.reset(new ExternalExecutionController(execution, source_,
                market_ == "SH" ? "SSE" : "SZE", inputs));
            decisions = external_; break;
        }
    }
    execution_.reset(new ProtectedExecution(
        decisions, healthy, source_, market_ == "SH" ? "SSE" : "SZE", universe));
    if (external_) {
        const std::weak_ptr<ProtectedExecution> weak = execution_;
        external_->set_gate([weak]() {
            const auto boundary = weak.lock(); return boundary && boundary->permits_new_orders();
        });
    }
    Json config = legacy_config;
    config["td_source_index"] = Json::array({execution_source});
    const std::string routing_key = market_ == "SH"
        ? "sse_order_routing" : "sze_order_routing";
    config[routing_key]["enabled"] = true;
    config[routing_key]["mode"] = "executor";

    for (Json::const_iterator it = inputs.begin(); it != inputs.end(); ++it) {
        const std::string code = it.key().substr(0, 6);
        InsParams params;
        params.static_position = position(it.value(), "static_position", false);
        params.last_position = position(it.value(), "last_position", true);
        const long long total = static_cast<long long>(params.static_position) +
                                params.last_position;
        if (total < 0 || total > std::numeric_limits<int>::max())
            throw std::runtime_error("strategy startup position out of range");
        if (finite_number(globals, "position_limit") * params.static_position > std::numeric_limits<int>::max())
            throw std::runtime_error("strategy position limit exceeds native range");
        std::unique_ptr<Instrument> instrument(new Instrument);
        instrument->strategy.reset(new ZStrategy(code, params, config, execution_));
        instruments_.insert(std::make_pair(code, std::move(instrument)));
    }
}

void StrategySession::set_ready(bool account, bool risk, bool execution) {
    execution_->set_ready(account, risk, execution);
}

void StrategySession::begin_stop() { execution_->begin_stop(); }

void StrategySession::on_signal(const std::string& code,
                                const MSMarketDataField& view,
                                double prediction, long received_time) {
    on_decision(code, view, [&]() {
        ++signals_;
        instruments_.at(code)->strategy->on_signal(&instruments_.at(code)->view, prediction, source_, received_time);
    });
}

void StrategySession::on_decision(const std::string& code, const MSMarketDataField& view,
                                  const std::function<void()>& t0) {
    const Instruments::iterator found = instruments_.find(code);
    if (found == instruments_.end())
        throw std::runtime_error("strategy signal outside configured universe");
    Instrument& instrument = *found->second;
    instrument.view = view;
    instrument.have_view = true;
    try {
        if (external_) external_->begin_round(code, instrument.view);
        if (t0) t0();
        if (external_) external_->finish_round();
    } catch (...) {
        if (external_) external_->abort_round();
        throw;
    }
}

bool StrategySession::on_order(const LFRtnOrderField&, int, short, long) {
    return false; // Legacy reports lack an immutable connection epoch and fill coverage.
}

void StrategySession::on_timer(std::uint64_t time,
                              const std::function<bool(const std::string&)>& instrument_gate) {
    if (!external_ || time >= 86400000000ULL || time / 1000000ULL == last_timer_second_) return;
    last_timer_second_ = time / 1000000ULL;
    for (const auto& item : instruments_) {
        if (!item.second->have_view) continue;
        const auto& last = item.second->view;
        if (!std::isfinite(last.MarketTime) || last.MarketTime < 0 || last.MarketTime >= 240000000.0) continue;
        const std::uint64_t encoded = static_cast<std::uint64_t>(last.MarketTime);
        const std::uint64_t observed = (encoded / 10000000 * 3600 +
            encoded / 100000 % 100 * 60 + encoded / 1000 % 100) * 1000000 + encoded % 1000 * 1000;
        if (observed > time) continue;
        MSMarketDataField view = last;
        view.MarketTime = ((time / 3600000000ULL * 10000 + time / 60000000ULL % 60 * 100 +
            time / 1000000ULL % 60) * 1000 + time / 1000 % 1000);
        if (time - observed > 5000000ULL || (instrument_gate && !instrument_gate(item.first))) {
            view.ms_market_data.ms_market_data[BidPrice1Index + 2] = 0;
            view.ms_market_data.ms_market_data[AskPrice1Index + 2] = 0;
        }
        // Keep the stored quote timestamp when a timer uses the last book.
        try {
            external_->begin_round(item.first, view); external_->finish_round();
        } catch (...) { external_->abort_round(); throw; }
    }
}

bool StrategySession::on_trade(const LFRtnTradeField&, int, short, long) {
    return false;
}

const MSMarketDataField* StrategySession::last_view(
    const std::string& instrument) const {
    const Instruments::const_iterator found = instruments_.find(instrument);
    return found == instruments_.end() || !found->second->have_view
        ? 0 : &found->second->view;
}

void StrategySession::sync_startup_positions(
    const std::map<std::string, std::pair<int, int> >& total_available) {
    execution_->set_ready(false, false, false);
    if (signals_ != 0)
        throw std::runtime_error("cannot sync startup positions after session activity");
    if (total_available.size() != instruments_.size())
        throw std::runtime_error("startup positions must cover the full strategy universe");
    for (Instruments::const_iterator it = instruments_.begin();
         it != instruments_.end(); ++it) {
        const std::map<std::string, std::pair<int, int> >::const_iterator position =
            total_available.find(it->first);
        if (position == total_available.end() || position->second.first < 0 ||
            position->second.second < 0 ||
            position->second.second > position->second.first)
            throw std::runtime_error("invalid or incomplete startup positions");
    }
    for (Instruments::iterator it = instruments_.begin();
         it != instruments_.end(); ++it) {
        const std::pair<int, int>& position = total_available.find(it->first)->second;
        it->second->strategy->sync_startup_position(position.first, position.second);
    }
}

}  // namespace strategy_runtime
