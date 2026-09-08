#include "sse/runtime/sse_strategy_session.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace sse_strategy {
namespace {
typedef nlohmann::json Json;

double market_time(std::uint64_t micros) {
    const std::uint64_t seconds = micros / 1000000ULL;
    return static_cast<double>((seconds / 3600ULL * 10000ULL +
        seconds / 60ULL % 60ULL * 100ULL + seconds % 60ULL) * 1000ULL + micros / 1000ULL % 1000ULL);
}

}

Session::Session(const Json& legacy_config, short execution_source,
        const std::shared_ptr<StrategyExecution>& execution, const std::function<bool()>& healthy)
    : core_(), last_exchange_us_(), pending_batch_outputs_(), batch_end_mode_(false),
      single_flight_(true), stale_signal_drops_(0) {
    Json config = legacy_config;
    if (config.at("market") != "SH") throw std::runtime_error("SSE session requires market SH");
    if (config.count("sse_test_order") && config.at("sse_test_order").value("enabled", false))
        throw std::runtime_error("stream strategy does not enable diagnostic test orders");
    single_flight_ = !config.count("sse_single_flight") || config.at("sse_single_flight").get<bool>();
    // Routing below selects the injected executor, never a broker by itself.
    config["td_source_index"] = Json::array({execution_source});
    config["sse_order_routing"]["enabled"] = true;
    config["sse_order_routing"]["mode"] = "executor";
    core_.reset(new strategy_runtime::StrategySession(
        config, "SH", execution_source, execution, healthy));
}

void Session::set_ready(bool account, bool risk, bool execution) {
    core_->set_ready(account, risk, execution);
}
void Session::begin_stop() { core_->begin_stop(); }

void Session::on_output(const sse_stream::Output& output) {
    try { process_output(output); }
    catch (...) { begin_stop(); throw; }
}

void Session::process_output(const sse_stream::Output& output) {
    if (output.kind == sse_stream::kBatchEndOutput) {
        batch_end_mode_ = true;
        flush_batch_outputs();
        return;
    }
    const bool hardware_batch =
        (output.kind == sse_stream::kTickOutput
             ? output.tick.provenance.processing_contract
             : output.kind == sse_stream::kSnapshotOutput
                 ? output.snapshot.provenance.processing_contract : sse_stream::kSoftwarePerInstrumentV2) ==
        sse_stream::kHardwareBatchV3;
    if (batch_end_mode_ || hardware_batch) {
        batch_end_mode_ = true;
        pending_batch_outputs_.push_back(output);
        return;
    }
    process_prediction_output(output);
}

void Session::flush_batch_outputs() {
    const std::vector<sse_stream::Output> pending = pending_batch_outputs_;
    pending_batch_outputs_.clear();
    for (std::vector<sse_stream::Output>::const_iterator it = pending.begin();
         it != pending.end(); ++it)
        process_prediction_output(*it);
}

void Session::process_prediction_output(const sse_stream::Output& output) {
    const bool tick = output.kind == sse_stream::kTickOutput;
    if (!tick && output.kind != sse_stream::kSnapshotOutput) throw std::runtime_error("unknown strategy output kind");
    const sse_hybrid_model::Prediction& prediction = tick ? output.tick.prediction : output.snapshot.prediction;
    if (!(tick ? output.tick.prediction_valid : output.snapshot.prediction_valid) || !prediction.selected) return;
    const std::uint64_t exchange_us = tick ? output.tick.event.time_of_day_micros : output.snapshot.snapshot.time_of_day_micros;
    const std::string& code = tick ? output.tick.event.security_id : output.snapshot.snapshot.security_id;
    const bool tick_window = exchange_us >= 34500000000ULL;
    if (exchange_us < 34200000000ULL || exchange_us >= 86400000000ULL || tick != tick_window ||
        prediction.selected_source != (tick ? sse_hybrid_model::kTickSource : sse_hybrid_model::kSnapshotSource) ||
        !std::isfinite(prediction.selected_pred)) throw std::runtime_error("invalid selected strategy signal");
    const std::map<std::string, std::uint64_t>::const_iterator previous =
        last_exchange_us_.find(code);
    if (previous != last_exchange_us_.end() && exchange_us < previous->second) {
        const sse_stream::ProcessingContract contract = tick ?
            output.tick.provenance.processing_contract : output.snapshot.provenance.processing_contract;
        if (contract == sse_stream::kHardwareBatchV3) {
            // Independent UDP subscriptions can complete cuts in a different
            // exchange-time order. The model has already consumed every row;
            // an obsolete selected signal must not move trading backwards.
            ++stale_signal_drops_;
            return;
        }
        throw std::runtime_error("strategy exchange time moved backwards");
    }
    if (previous != last_exchange_us_.end() && exchange_us == previous->second) return;
    MSMarketDataField fresh = {MSMarketData()};
    double* values = fresh.ms_market_data.ms_market_data.data();
    std::fill(values, values + BASIC_FIELD_NUM, 0.0);
    values[InstrumentIDIndex] = std::strtol(code.c_str(), 0, 10);
    values[MarketTimeIndex] = market_time(exchange_us);
    if (tick) {
        if (output.tick.bid_levels.size() != 10 || output.tick.ask_levels.size() != 10)
            throw std::runtime_error("strategy requires a ten-level tick cut");
        for (std::size_t i = 0; i < 10; ++i) {
            values[BidPrice1Index + i] = output.tick.bid_levels[i].price_raw / 1000.0;
            values[AskPrice1Index + i] = output.tick.ask_levels[i].price_raw / 1000.0;
            values[BidVolume1Index + i] = output.tick.bid_levels[i].quantity / 1000.0;
            values[AskVolume1Index + i] = output.tick.ask_levels[i].quantity / 1000.0;
        }
        values[LastPriceIndex] = output.tick.last_trade_price;
        values[VolumeIndex] = output.tick.total_trade_volume;
        values[TurnoverIndex] = output.tick.total_trade_turnover;
        values[AppSeqIndex] = output.tick.event.tick_index;
    } else {
        const sse_live::Snapshot& snapshot = output.snapshot.snapshot;
        for (std::size_t i = 0; i < 5; ++i) {
            values[BidPrice1Index + i] = snapshot.bid_prices[i];
            values[AskPrice1Index + i] = snapshot.ask_prices[i];
            values[BidVolume1Index + i] = snapshot.bid_volumes[i];
            values[AskVolume1Index + i] = snapshot.ask_volumes[i];
        }
        values[LastPriceIndex] = snapshot.last_price;
        values[VolumeIndex] = snapshot.volume;
        values[TurnoverIndex] = snapshot.turnover;
        values[AppSeqIndex] = snapshot.sequence;
    }
    for (int i = 0; i < BASIC_FIELD_NUM; ++i)
        if (!std::isfinite(values[i]) || values[i] < 0) throw std::runtime_error("invalid strategy book view");
    if (values[BidPrice1Index] <= 0 || values[AskPrice1Index] < values[BidPrice1Index] ||
        values[BidVolume1Index] <= 0 || values[AskVolume1Index] <= 0) return;
    values[MidPriceIndex] = (values[BidPrice1Index] + values[AskPrice1Index]) * 0.5;
    if (values[LastPriceIndex] <= 0) values[LastPriceIndex] = values[MidPriceIndex];
    last_exchange_us_[code] = exchange_us;
    if (single_flight_ && core_->execution()->has_working_order(code)) return;
    core_->on_signal(code, fresh, prediction.selected_pred,
                     core_->execution()->now_ns());
}

bool Session::on_order(const LFRtnOrderField& order, int request_id, short source, long received_ns) {
    return core_->on_order(order, request_id, source, received_ns);
}

bool Session::on_trade(const LFRtnTradeField& trade, int request_id, short source, long received_ns) {
    return core_->on_trade(trade, request_id, source, received_ns);
}

const MSMarketDataField* Session::last_view(const std::string& instrument) const {
    return core_->last_view(instrument);
}

}  // namespace sse_strategy
