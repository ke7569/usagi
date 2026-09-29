#include "sze/runtime/SzeStrategySession.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace sze_strategy {

Session::Session(const nlohmann::json& legacy_config, short execution_source,
                 const std::shared_ptr<StrategyExecution>& execution,
                 const std::function<bool()>& healthy)
    : core_(legacy_config, "SZ", execution_source, execution, healthy) {}

void Session::set_ready(bool account, bool risk, bool execution) {
    core_.set_ready(account, risk, execution);
}

void Session::begin_stop() { core_.begin_stop(); }

std::string Session::normalize_code(const std::string& instrument) {
    const std::size_t dot = instrument.find('.');
    const std::string code = dot == std::string::npos
        ? instrument : instrument.substr(0, dot);
    if (instrument.find('.') != std::string::npos &&
        (instrument.size() != 9U || instrument.substr(6) != ".SZ"))
        throw std::runtime_error("SZE strategy sample has non-SZ instrument");
    if (code.size() != 6U) throw std::runtime_error("SZE strategy sample has invalid instrument");
    for (std::size_t i = 0; i < code.size(); ++i)
        if (code[i] < '0' || code[i] > '9')
            throw std::runtime_error("SZE strategy sample has invalid instrument");
    if (code.compare(0, 2, "00") != 0 && code.compare(0, 2, "30") != 0)
        throw std::runtime_error("SZE strategy sample has non-equity instrument");
    return code;
}

double Session::market_time(std::int64_t exchange_time_us) {
    const std::int64_t day_us = 86400000000LL;
    std::int64_t tod = exchange_time_us % day_us;
    if (tod < 0) tod += day_us;
    const std::int64_t hour = tod / 3600000000LL;
    tod %= 3600000000LL;
    const std::int64_t minute = tod / 60000000LL;
    tod %= 60000000LL;
    const std::int64_t second = tod / 1000000LL;
    const std::int64_t millisecond = (tod % 1000000LL) / 1000LL;
    return static_cast<double>(hour * 10000000LL + minute * 100000LL +
                               second * 1000LL + millisecond);
}

bool Session::valid_sample_view(const mix153060::Sample& sample) {
    const double last = sample.last_price > 0.0 ? sample.last_price : sample.mid_price;
    if (sample.exchange_time_us <= 0 || sample.local_time_us <= 0 || !std::isfinite(sample.volume) ||
        !std::isfinite(sample.turnover) || !std::isfinite(sample.mid_price) ||
        !std::isfinite(last) || last <= 0.0 || sample.mid_price <= 0.0 ||
        sample.volume < 0.0 || sample.turnover < 0.0)
        return false;
    for (std::size_t level = 0; level < 10U; ++level) {
        if (!std::isfinite(sample.bid_price[level]) ||
            !std::isfinite(sample.ask_price[level]) ||
            !std::isfinite(sample.bid_volume[level]) ||
            !std::isfinite(sample.ask_volume[level]) ||
            sample.bid_price[level] < 0.0 || sample.ask_price[level] < 0.0 ||
            sample.bid_volume[level] < 0.0 || sample.ask_volume[level] < 0.0)
            return false;
    }
    return sample.bid_price[0] > 0.0 && sample.ask_price[0] > 0.0 &&
           sample.bid_volume[0] > 0.0 && sample.ask_volume[0] > 0.0;
}

MSMarketDataField Session::make_view(const std::string& code,
                                     const mix153060::Sample& sample) {
    MSMarketDataField result = {MSMarketData()};
    result.ms_market_data.ms_market_data[InstrumentIDIndex] = std::strtod(code.c_str(), 0);
    result.ms_market_data.ms_market_data[MarketTimeIndex] = market_time(sample.exchange_time_us);
    result.ms_market_data.ms_market_data[LastPriceIndex] = sample.last_price > 0.0
        ? sample.last_price : sample.mid_price;
    result.ms_market_data.ms_market_data[MidPriceIndex] = sample.mid_price;
    result.ms_market_data.ms_market_data[VolumeIndex] = sample.volume;
    result.ms_market_data.ms_market_data[TurnoverIndex] = sample.turnover;
    for (std::size_t level = 0; level < 10U; ++level) {
        result.ms_market_data.ms_market_data[BidVolume1Index + level] = sample.bid_volume[level];
        result.ms_market_data.ms_market_data[AskVolume1Index + level] = sample.ask_volume[level];
        result.ms_market_data.ms_market_data[BidPrice1Index + level] = sample.bid_price[level];
        result.ms_market_data.ms_market_data[AskPrice1Index + level] = sample.ask_price[level];
    }
    result.ms_market_data.ms_market_data[AppSeqIndex] = static_cast<double>(sample.app_sequence);
    return result;
}

void Session::on_output(const sze_stream::ProcessedSample& output) {
    try {
        const bool prediction_valid = output.prediction_valid && std::isfinite(output.prediction);
        if (!prediction_valid && !core_.has_external_execution()) return;
        const std::string code = normalize_code(output.sample.instrument);
        if (!valid_sample_view(output.sample)) return;
        const MSMarketDataField view = make_view(code, output.sample);
        if (prediction_valid)
            core_.on_signal(code, view, output.prediction, static_cast<long>(output.sample.local_time_us));
        else core_.on_decision(code, view, std::function<void()>());
    } catch (...) {
        begin_stop();
        throw;
    }
}

bool Session::on_order(const LFRtnOrderField& order, int request_id,
                       short source, long received_ns) {
    return core_.on_order(order, request_id, source, received_ns);
}

bool Session::on_trade(const LFRtnTradeField& trade, int request_id,
                       short source, long received_ns) {
    return core_.on_trade(trade, request_id, source, received_ns);
}

const MSMarketDataField* Session::last_view(const std::string& instrument) const {
    return core_.last_view(instrument);
}

std::uint64_t Session::signals() const { return core_.signals(); }

std::shared_ptr<strategy_runtime::ProtectedExecution> Session::execution() const {
    return core_.execution();
}

void Session::sync_startup_positions(
    const std::map<std::string, std::pair<int, int> >& total_available) {
    core_.sync_startup_positions(total_available);
}

}  // namespace sze_strategy
