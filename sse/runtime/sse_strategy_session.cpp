#include "sse/runtime/sse_strategy_session.h"
#ifdef SSE_REPLAY_PROBE
extern void sse_strategy_signal_probe(const sse_stream::Output&);
#endif
#include "common/oms/OrderLatency.h"
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
      single_flight_(true) {
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
    if (config.value("model_version", std::string("legacy")) == "v0.6")
        v06_.reset(new sse_v06::Strategy(config, execution_source, core_->execution()));
}

void Session::set_ready(bool account, bool risk, bool execution) {
    core_->set_ready(account, risk, execution);
}
void Session::begin_stop() { core_->begin_stop(); }

void Session::on_output(const sse_stream::Output& output) {
    try { process_output(output); }
    catch (...) { begin_stop(); throw; }
}

void Session::on_closed_prediction(const sse_stream::Output& output) {
    try {
        if(!v06_ || output.kind!=sse_stream::kTickOutput ||
           !output.tick.provenance.batch_id || !output.tick.provenance.batch_emitted_ns)
            throw std::runtime_error("independent signal requires v06 input BatchEnd");
        process_prediction_output(output);
    } catch(...) {begin_stop();throw;}
}

void Session::on_completed_batch(const std::vector<sse_stream::Output>& outputs) {
    try {
        if (outputs.empty() || outputs.back().kind != sse_stream::kBatchEndOutput)
            throw std::runtime_error("completed strategy batch requires BatchEnd");
        // Snapshot references historically became visible before BatchEnd
        // released any signals. Keep that ordering for an owned batch too.
        if (v06_) {
            for (const auto& output : outputs)
                if (output.kind == sse_stream::kSnapshotOutput)
                    v06_->reference(output.snapshot.snapshot.security_id,
                                    output.snapshot.snapshot.open_price);
        }
        batch_end_mode_ = true;
        // The raw/serial entry can still have snapshots preceding this close.
        flush_batch_outputs();
        for (std::size_t i = 0; i + 1 < outputs.size(); ++i)
            process_prediction_output(outputs[i]);
    } catch (...) { begin_stop(); throw; }
}

nlohmann::json Session::live_latency()const {
    const auto interval_count=latency_count_-last_latency_count_;
    auto percentile=[&](unsigned p,bool interval){
        const auto count=interval?interval_count:latency_count_;
        if(!count)return std::uint64_t(0);
        const auto target=(count*p+99)/100;std::uint64_t sum=0;
        for(std::size_t i=0;i<latency_bins_.size();++i){
            sum+=latency_bins_[i]-(interval?last_latency_bins_[i]:0);
            if(sum>=target)return i<10000?std::uint64_t(i+1):((interval?interval_latency_max_:latency_max_)+999)/1000;
        }
        return std::uint64_t(0);
    };
    nlohmann::json result={{"scope","software_receive_to_actual_strategy_entry_live_only"},{"count",latency_count_},
        {"mean_us",latency_count_?double(latency_sum_)/latency_count_/1000:0},{"p50_upper_us",percentile(50,false)},
        {"p95_upper_us",percentile(95,false)},{"p99_upper_us",percentile(99,false)},{"max_us",latency_max_/1000.0},
        {"at_least_1ms",latency_over_ms_}};
    result["since_previous_status"]={{"count",interval_count},
        {"mean_us",interval_count?double(latency_sum_-last_latency_sum_)/interval_count/1000:0},
        {"p50_upper_us",percentile(50,true)},{"p95_upper_us",percentile(95,true)},{"p99_upper_us",percentile(99,true)},
        {"max_us",interval_latency_max_/1000.0},{"at_least_1ms",latency_over_ms_-last_latency_over_ms_}};
    last_latency_count_=latency_count_;last_latency_sum_=latency_sum_;last_latency_over_ms_=latency_over_ms_;
    last_latency_bins_=latency_bins_;interval_latency_max_=0;
    return result;
}

void Session::process_output(const sse_stream::Output& output) {
    if (v06_ && output.kind == sse_stream::kSnapshotOutput) {
        v06_->reference(output.snapshot.snapshot.security_id, output.snapshot.snapshot.open_price);
        // v0.6 uses snapshots as opening-price references, not predictions.
        // An unselected snapshot has no work left at the commit boundary.
        if (!output.snapshot.prediction_valid || !output.snapshot.prediction.selected) return;
    }
    if (output.kind == sse_stream::kBatchEndOutput) {
        batch_end_mode_ = true;
        flush_batch_outputs();
        return;
    }
    const bool hardware_timestamped =
        (output.kind == sse_stream::kTickOutput
             ? output.tick.provenance.timestamp_flags
             : output.kind == sse_stream::kSnapshotOutput
                 ? output.snapshot.provenance.timestamp_flags : 0U) &
        deepwin_market_data::kHardwareTimestampRequested;
    if (batch_end_mode_ || hardware_timestamped) {
        batch_end_mode_ = true;
        if(pending_batch_size_==pending_batch_outputs_.size())pending_batch_outputs_.push_back(output);
        else pending_batch_outputs_[pending_batch_size_]=output;
        ++pending_batch_size_;
        return;
    }
    process_prediction_output(output);
}

void Session::flush_batch_outputs() {
    processing_batch_outputs_.swap(pending_batch_outputs_);
    const std::size_t count=pending_batch_size_;pending_batch_size_=0;
    for(std::size_t i=0;i<count;++i)process_prediction_output(processing_batch_outputs_[i]);
}

void Session::process_prediction_output(const sse_stream::Output& output) {
    const bool tick = output.kind == sse_stream::kTickOutput;
    if (!tick && output.kind != sse_stream::kSnapshotOutput) throw std::runtime_error("unknown strategy output kind");
    const sse_hybrid_model::Prediction& prediction = tick ? output.tick.prediction : output.snapshot.prediction;
    if (!(tick ? output.tick.prediction_valid : output.snapshot.prediction_valid) || !prediction.selected) return;
    const std::uint64_t exchange_us = tick ? output.tick.event.time_of_day_micros : output.snapshot.snapshot.time_of_day_micros;
    const std::string& code = tick ? output.tick.event.security_id : output.snapshot.snapshot.security_id;
    // A later snapshot in the same hardware batch may invalidate daily
    // prices after this prediction was queued. Check again at dispatch.
    if (instrument_gate_ && !instrument_gate_(code)) return;
    const bool tick_window = prediction.multi_head || exchange_us >= 34500000000ULL;
    if (exchange_us < 34200000000ULL || exchange_us >= 86400000000ULL || tick != tick_window ||
        prediction.selected_source != (tick ? sse_hybrid_model::kTickSource : sse_hybrid_model::kSnapshotSource) ||
        !std::isfinite(prediction.selected_pred)) throw std::runtime_error("invalid selected strategy signal");
    const std::map<std::string, std::uint64_t>::const_iterator previous =
        last_exchange_us_.find(code);
    if (previous != last_exchange_us_.end() && exchange_us < previous->second)
        throw std::runtime_error("strategy exchange time moved backwards");
    if (!prediction.multi_head && previous != last_exchange_us_.end() && exchange_us == previous->second) return;
    // Signals run serially on the strategy owner; clear and reuse its view.
    MSMarketDataField& fresh = signal_view_;
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
    if (v06_) {
        if (!prediction.multi_head) throw std::runtime_error("v06 strategy requires four model heads");
#ifdef SSE_REPLAY_PROBE
        sse_strategy_signal_probe(output);
#endif
        const auto actual_signal_ns=order_latency::now_ns();
        if(latency_enabled_ && tick && actual_signal_ns>=output.tick.provenance.monotonic_ns) {
            const auto delta=actual_signal_ns-output.tick.provenance.monotonic_ns;
            ++latency_count_;latency_sum_+=delta;latency_max_=std::max(latency_max_,delta);
            interval_latency_max_=std::max(interval_latency_max_,delta);latency_over_ms_+=delta>=1000000;
            ++latency_bins_[std::min<std::uint64_t>(delta/1000,10000)];
        }
        v06_->on_signal(code,fresh,prediction.heads,exchange_us,
            tick?output.tick.provenance.monotonic_ns:output.snapshot.provenance.monotonic_ns,actual_signal_ns);
        return;
    }
    if (prediction.multi_head) throw std::runtime_error("v06 model requires v06 strategy");
    if (single_flight_ && !prediction.multi_head && core_->execution()->has_working_order(code)) return;
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
