#ifndef T0_STREAM_PROCESSING_CLI_H
#define T0_STREAM_PROCESSING_CLI_H

#include "common/config/StreamConfigJson.h"
#include "common/config/StreamInputConfig.h"
#include <boost/crc.hpp>
#include <cmath>
#include <memory>
#include <limits>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <ctime>
#include "common/execution/OmsStrategyExecution.h"

#ifdef T0_STREAM_SZE
#include "sze/runtime/sze_stream_processor.h"
#include "sze/runtime/sze_recovery_driver.h"
#include "sze/runtime/SzeStrategySession.h"
#include "sze/market_data/SZERecoverable.h"
#else
#include "sse/runtime/sse_stream_processor.h"
#include "sse/factors/auction59_sidecar.h"
#include "sse/runtime/sse_strategy_session.h"
#endif

#ifdef T0_STREAM_SZE
namespace sze_application {
typedef sze_strategy::Session MarketStrategySession;
#else
namespace sse_application {
typedef sse_strategy::Session MarketStrategySession;
#endif

// Shared application assembly for CLI and owned runtime API. No broker SDK.
class StreamProcessingCli {
    typedef nlohmann::json Json;
    static std::string string(const Json& object, const char* key) {
        const Json& value = object.at(key);
        if (!value.is_string()) throw std::runtime_error(std::string("profile string required: ") + key);
        return value.get<std::string>();
    }
    static double number(const Json& object, const char* key) {
        const Json& value = object.at(key);
        if (!value.is_number() || !std::isfinite(value.get<double>()))
            throw std::runtime_error(std::string("profile finite number required: ") + key);
        return value.get<double>();
    }
    static std::string absolute(const std::string& path) {
        char* resolved = ::realpath(path.c_str(), 0);
        if (!resolved) throw std::runtime_error("cannot resolve replay directory");
        const std::string result(resolved);
        std::free(resolved);
        return result;
    }
    static void subset(const Json& value, const Json& expected) {
        if (!value.is_object()) throw std::runtime_error("invalid processing contract declaration");
        for (auto it = value.begin(); it != value.end(); ++it) {
            const auto found = expected.find(it.key());
            if (found == expected.end()) throw std::runtime_error("unknown processing contract field: " + it.key());
            if (found->is_object()) subset(*it, *found);
            else if (it->is_boolean() != found->is_boolean() || *it != *found)
                throw std::runtime_error("unsupported processing contract value: " + it.key());
        }
    }
public:
    StreamProcessingCli(const std::string& path, bool capture,
                        const std::string& recording, std::size_t channels,
                        const std::function<bool()>& healthy = std::function<bool()>(),
                        const std::string& input_driver = "raw")
        : profile_(load_stream_json(path)), rows_(0), predictions_(0), last_sequence_(0),
          order_intents_(0), cancel_intents_(0), input_driver_(input_driver),
          stopped_(false), reached_live_(false) {
        std::set<std::string> fields = {"schema_version", "market", "execution", "processing_mode",
            "processing_contract", "trading_day", "processing_sha256", "environment", "prediction", "instruments"};
        if (profile_.count("strategy_runtime")) fields.insert("strategy_runtime");
#ifdef T0_STREAM_SZE
        if (input_driver_ != "raw") {
            fields.insert("input_driver"); fields.insert("recovery");
        }
#else
        if (input_driver_ != "raw") throw std::runtime_error("SSE does not accept SZE recovery input");
#endif
        if (profile_.value("input_driver", std::string("raw")) != input_driver_)
            throw std::runtime_error("processing input driver mismatch");
        if (!profile_.is_object() || profile_.size() != fields.size()) throw std::runtime_error("invalid processing profile fields");
        for (auto it = profile_.begin(); it != profile_.end(); ++it)
            if (!fields.count(it.key())) throw std::runtime_error("unknown processing profile field: " + it.key());
        const std::string fingerprint = string(profile_, "processing_sha256");
        if (fingerprint.size() != 64 || fingerprint.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("invalid profile processing fingerprint");
        if (number(profile_, "schema_version") != 1 || string(profile_, "execution") != "disabled")
            throw std::runtime_error("only schema v1 execution-disabled processing profiles are supported");
        const std::string mode = string(profile_, "processing_mode");
        if (mode != "factors-only" && mode != "prediction") throw std::runtime_error("invalid processing mode");
        const bool factors_only = mode == "factors-only";
        if (factors_only && profile_.count("strategy_runtime"))
            throw std::runtime_error("strategy intents require model predictions");
        const Json& environment = profile_.at("environment");
        if (string(environment, "execution") != "disabled" ||
            string(environment, "mode") != (capture ? "live" : "replay") ||
            string(environment, "clock") != (capture ? "host" : "virtual"))
            throw std::runtime_error("processing profile environment disagrees with input driver");
        if (!capture && (string(environment, "record_format") != (input_driver_ == "sze-journal" ? "canonical-events" : "t0md-v1") ||
                         string(environment, "time_basis") != (input_driver_ == "sze-journal" ? "exchange" : "recorded-receive") ||
                         absolute(string(environment, "recording")) != absolute(recording)))
            throw std::runtime_error("replay input differs from processing profile");
        const double trading_day = number(profile_, "trading_day");
        if (trading_day < 20000101 || trading_day > 99991231 || trading_day != std::floor(trading_day))
            throw std::runtime_error("invalid profile trading day");
        const Json& inputs = profile_.at("instruments");
        if (!inputs.is_array() || inputs.empty()) throw std::runtime_error("profile instruments required");
        const Json& prediction = profile_.at("prediction");
        std::string error;
#ifdef T0_STREAM_SZE
        if (string(profile_, "market") != "SZ" || string(profile_, "processing_contract") != "sze-mix153060-v04")
            throw std::runtime_error("this binary requires the SZE mix153060 profile");
        if (input_driver_ != "raw") load_recovery_config(recording, capture, trading_day);
        std::vector<mix153060::StaticInputs> instruments;
        for (const Json& input : inputs) {
            mix153060::StaticInputs value;
            value.instrument = string(input, "instrument");
            if (number(input, "trading_date") != trading_day) throw std::runtime_error("mixed profile trading dates");
            value.trading_date = static_cast<int32_t>(trading_day);
            value.average_amount = number(input, "average_amount");
            value.turnover_threshold = number(input, "turnover_threshold");
            value.free_share = number(input, "free_share");
            value.pre_close = number(input, "pre_close");
            value.upper_limit = number(input, "upper_limit");
            value.lower_limit = number(input, "lower_limit");
            value.history_volatility_20d = number(input, "history_volatility_20d");
            instruments.push_back(value);
        }
        if (!factors_only && !model_.load(string(prediction, "model_path"), &error))
            throw std::runtime_error(error);
        processor_.reset(new sze_stream::SzeStreamProcessor(instruments, factors_only ? 0 : &model_, channels,
            [&](const sze_stream::ProcessedSample& output) {
                ++rows_;
                if (output.prediction_valid) ++predictions_;
                last_sequence_ = output.ingress_sequence;
                crc_.process_bytes(output.sample.factors.data(), sizeof(float) * output.sample.factors.size());
                if (strategy_) strategy_->on_output(output);
            }, input_driver_ == "raw" ? 88 : recovery_config_.source_id));
        if (input_driver_ != "raw") {
            recovery_.reset(new sze_stream::SzeRecoveryDriver(processor_.get()));
            recovery_->set_event_observer([this](const sze_recovery::CanonicalEvent& event) {
                if (oms_ && input_driver_ == "sze-handoff") advance_clock(event.receive_mono_ns);
            });
        }
#else
        if (string(profile_, "market") != "SH" || string(profile_, "processing_contract") != "sse-per-instrument-v2")
            throw std::runtime_error("this binary requires the SSE per-instrument-v2 profile");
        const Json sampling = Json::parse(R"json({"mode":"trailing-edge-one-shot","threshold_ns":100000,
            "comparison":"strict-greater-than","clock":"CLOCK_MONOTONIC","candidate_event":"CompleteOrderBookSH Level2",
            "activity_scope":"per-instrument-sse-book-update","same_exchange_time_policy":"at-most-one-sample",
            "initial_window":"first-valid-book-at-or-after-open","sequence_gap_policy":"fail-closed","periodic_md":false,
            "shutdown_flush":false,"standard_gate":{"turnover_threshold_source":"daily-instrument-static-required",
            "exchange_time_trigger_us":100000000,"mid_change_epsilon":0.000001,"min_volume_change":100}})json");
        const Json routing = Json::parse(R"json({"clock":"exchange-time-of-day-micros",
            "snapshot_selected_window":"[09:30:00,09:35:00)","tick_selected_window":"[09:35:00,24:00:00)",
            "tick_warm_before_switch":true,"silent_fallback":false})json");
        if (prediction.count("sampling")) subset(prediction.at("sampling"), sampling);
        if (prediction.count("routing")) subset(prediction.at("routing"), routing);
        (void)channels;
        sse_tick::DailyStaticMetadataMap metadata;
        for (const Json& input : inputs) {
            sse_tick::DailyStaticMetadata value;
            const std::string instrument = string(input, "instrument");
            if (number(input, "trading_date") != trading_day) throw std::runtime_error("mixed profile trading dates");
            value.date = static_cast<std::uint32_t>(trading_day);
            value.has_date = true;
            value.avg_amount = number(input, "average_amount"); value.has_avg_amount = true;
            value.turnover_threshold = number(input, "turnover_threshold"); value.has_turnover_threshold = true;
            value.free_share = number(input, "free_share"); value.has_free_share = true;
            value.pre_close = number(input, "pre_close"); value.has_pre_close = true;
            value.limit_price = number(input, "upper_limit"); value.has_limit_price = true;
            value.stop_price = number(input, "lower_limit"); value.has_stop_price = true;
            if (!metadata.insert(std::make_pair(instrument, value)).second)
                throw std::runtime_error("duplicate profile instrument");
        }
        if (!factors_only) {
            if (!model_.load(string(prediction, "model_path"),
                             string(prediction, "snapshot_baseline_model_path"),
                             string(prediction, "snapshot_baseline_scaler_path"),
                             string(prediction, "snapshot_auction59_model_path"),
                             string(prediction, "snapshot_auction59_scaler_path"), &error))
                throw std::runtime_error(error);
            if (!sse_auction59::load_csv(string(prediction, "snapshot_auction59_factors_path"), &auction_, &error))
                throw std::runtime_error(error);
            for (const auto& entry : metadata) {
                const auto found = auction_.find(entry.first);
                if (found == auction_.end() || found->second.size() != 59)
                    throw std::runtime_error("missing Auction59 factors for " + entry.first);
                for (float value : found->second) if (!std::isfinite(value))
                    throw std::runtime_error("non-finite Auction59 factors for " + entry.first);
            }
        }
        processor_.reset(new sse_stream::SseStreamProcessor(metadata, factors_only ? 0 : &model_, factors_only,
            [&](const sse_stream::Output& output) {
                ++rows_;
                if (output.kind == sse_stream::kTickOutput) {
                    if (output.tick.prediction_valid) ++predictions_;
                    last_sequence_ = output.tick.provenance.stream_sequence;
                    crc_.process_bytes(output.tick.factors.values.data(), sizeof(float) * output.tick.factors.values.size());
                } else {
                    if (output.snapshot.prediction_valid) ++predictions_;
                    last_sequence_ = output.snapshot.provenance.stream_sequence;
                    crc_.process_bytes(output.snapshot.snapshot36.data(), sizeof(float) * output.snapshot.snapshot36.size());
                }
                if (strategy_) strategy_->on_output(output);
            }, [&](const std::string& instrument, std::uint64_t, std::vector<float>* values, std::string*) {
                const auto found = auction_.find(instrument);
                if (found == auction_.end()) return false;
                *values = found->second;
                return true;
            }));
#endif
        initialize_strategy(healthy);
    }
    void on_event(const deepwin_market_data::StreamEvent& event) {
        try {
            advance_clock(event.monotonic_ns);
            processor_->on_event(event);
        }
        catch (...) { begin_stop(); throw; }
    }
    void begin_stop() {
        if (strategy_) strategy_->begin_stop();
        if (oms_) oms_->begin_stop();
        stopped_.store(true);
#ifdef T0_STREAM_SZE
        if (recovery_) recovery_->request_stop();
#endif
    }
    bool recovery_ready() const { return reached_live_.load() && !stopped_.load(); }
    bool recovery_available() const {
#ifdef T0_STREAM_SZE
        return recovery_ && recovery_->available();
#else
        return false;
#endif
    }
    bool processing_valid() const {
#ifdef T0_STREAM_SZE
        return processor_->available();
#else
        return !processor_->invalid();
#endif
    }
    bool run_recovery() {
#ifdef T0_STREAM_SZE
        if (!recovery_) throw std::runtime_error("recovery driver not configured");
        if (input_driver_ == "sze-journal")
            return recovery_->replay(recovery_config_, sze_stream::RecoveryTimeContext::analysis_exchange_time());
        timespec mono = {}, real = {};
        if (::clock_gettime(CLOCK_MONOTONIC, &mono) || ::clock_gettime(CLOCK_REALTIME, &real))
            throw std::runtime_error("cannot establish recovery receive-clock anchor");
        const auto context = sze_stream::RecoveryTimeContext::same_boot(
            static_cast<std::uint64_t>(mono.tv_sec) * 1000000000ULL + mono.tv_nsec,
            static_cast<std::uint64_t>(real.tv_sec) * 1000000000ULL + real.tv_nsec);
        if (!recovery_->replay_handoff(recovery_config_, context)) return false;
        reached_live_.store(true);
        while (!stopped_.load()) {
            if (!recovery_->poll_handoff()) {
                if (!recovery_->available()) return false;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        return true;
#else
        throw std::runtime_error("SSE recovery input is unsupported");
#endif
    }
    std::string recovery_error() const {
#ifdef T0_STREAM_SZE
        return recovery_ ? recovery_->error() : std::string();
#else
        return std::string();
#endif
    }
    Json summary() const {
        Json result;
        result["contract"] = profile_.at("processing_contract");
        result["mode"] = profile_.at("processing_mode");
        result["processing_sha256"] = profile_.at("processing_sha256");
        result["samples"] = rows_;
        result["predictions"] = predictions_;
        result["factor_crc32"] = crc_.checksum();
        result["last_ingress_sequence"] = last_sequence_;
        result["execution"] = "disabled";
        if (strategy_) {
            result["strategy"]["mode"] = "paper-intents";
            result["strategy"]["signals"] = strategy_->signals();
            result["strategy"]["order_intents"] = order_intents_;
            result["strategy"]["cancel_intents"] = cancel_intents_;
            result["strategy"]["intent_crc32"] = intent_crc_.checksum();
            result["strategy"]["fills_simulated"] = false;
            result["strategy"]["account_baseline"] = "configured-paper-snapshot";
            const oms::AccountView account = oms_->account();
            result["strategy"]["oms"]["orders"] = account.orders;
            result["strategy"]["oms"]["pending_orders"] = account.pending_orders;
            result["strategy"]["oms"]["reserved_money_units"] = account.cash_reserved;
            result["strategy"]["oms"]["rejections"] = account.rejections;
            result["strategy"]["oms"]["anomalies"] = account.anomalies;
            result["strategy"]["oms"]["audit_tail"] = Json::array();
            const auto audit = oms_->audit_events();
            for (std::size_t i = audit.size() > 16 ? audit.size() - 16 : 0; i < audit.size(); ++i)
                result["strategy"]["oms"]["audit_tail"].push_back(Json{{"order_id", audit[i].order_id},
                    {"time_ns", audit[i].time_ns}, {"type", audit[i].type}, {"detail", audit[i].detail}});
            result["strategy"]["account_reference"] = profile_.at("strategy_runtime").at("account_reference");
        }
#ifdef T0_STREAM_SZE
        if (recovery_) {
            result["recovery"]["driver"] = input_driver_;
            result["recovery"]["records"] = recovery_->stats().records;
            result["recovery"]["handoff_events"] = recovery_->stats().handoff_events;
            result["recovery"]["reached_live"] = reached_live_.load();
            result["recovery"]["analysis_only"] = input_driver_ == "sze-journal";
        }
#endif
        return result;
    }
private:
    void advance_clock(std::uint64_t ns) {
        if (!oms_) return;
        if (ns > static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
            throw std::runtime_error("strategy stream time out of range");
        oms_->advance_to(static_cast<long long>(ns));
    }
    void initialize_strategy(const std::function<bool()>& healthy) {
        if (!profile_.count("strategy_runtime")) return;
        const Json& runtime = profile_.at("strategy_runtime");
        if (!runtime.is_object() || runtime.size() != 4 ||
            string(runtime, "mode") != "paper-intents" || !healthy)
            throw std::runtime_error("invalid paper strategy runtime or missing health provider");
        const Json& legacy = runtime.at("legacy_config");
        const std::string suffix = string(profile_, "market") == "SH" ? ".SH" : ".SZ";
        if (number(legacy, "trading_day") != number(profile_, "trading_day") ||
            legacy.at("ins_params").size() != profile_.at("instruments").size())
            throw std::runtime_error("strategy and processor inputs differ");
        std::set<std::string> universe;
        for (const Json& input : profile_.at("instruments")) {
            const std::string code = string(input, "instrument");
            if (!legacy.at("ins_params").count(code + suffix))
                throw std::runtime_error("strategy and processor universes differ");
            universe.insert(code);
        }
        short source = 1; // Local paper namespace; never registered with a TD.
        if (legacy.count("td_source_index")) {
            const Json& sources = legacy.at("td_source_index");
            if (!sources.is_array() || sources.size() > 1)
                throw std::runtime_error("paper strategy requires at most one explicit execution source");
            if (!sources.empty()) {
                if (!sources[0].is_number_integer() || sources[0].get<long long>() <= 0 ||
                    sources[0].get<long long>() > 32767)
                    throw std::runtime_error("invalid paper execution source");
                source = sources[0].get<short>();
            }
        }
        const Json& settings = runtime.at("oms");
        stream_input::fields(settings, {"simulation_cash", "fee_reserve_per_order"});
        oms::Config config;
        config.scope.account.broker = "paper"; config.scope.account.account = string(runtime, "account_reference");
        config.scope.source = source; config.scope.day = static_cast<std::uint32_t>(number(profile_, "trading_day"));
        config.scope.gateway = "stream-paper"; config.instance = "stream-paper-" + string(profile_, "market");
        config.enabled = true;
        oms::Snapshot snapshot;
        if (!oms::money_from_double(number(settings, "simulation_cash"), &snapshot.free_cash) ||
            !oms::money_from_double(number(settings, "fee_reserve_per_order"), &config.limits.fee_reserve_per_order))
            throw std::runtime_error("OMS requires explicit fixed-point simulation cash and fee reserve");
        const std::string exchange = suffix == ".SH" ? "SSE" : "SZE";
        for (const Json& input : profile_.at("instruments")) {
            const std::string code = string(input, "instrument");
            const Json& parameters = legacy.at("ins_params").at(code + suffix);
            oms::InstrumentRules rules;
            if (!oms::money_from_double(number(input, "lower_limit"), &rules.lower_price) ||
                !oms::money_from_double(number(input, "upper_limit"), &rules.upper_price))
                throw std::runtime_error("OMS price bands are not representable");
            rules.lot = parameters.value("vol_unit", 100);
            if (parameters.count("max_order_size") &&
                !oms::money_from_double(number(parameters, "max_order_size"), &rules.max_order_notional))
                throw std::runtime_error("OMS order notional is not representable");
            const oms::Instrument instrument = {exchange, code}; config.instruments[instrument] = rules;
            oms::SnapshotPosition position; position.instrument = instrument;
            position.total = parameters.at("static_position").get<long long>() + parameters.at("last_position").get<long long>();
            position.free_sellable = position.total; snapshot.positions.push_back(position);
        }
        std::shared_ptr<oms::Backend> backend(new oms::PaperBackend([this](const oms::Command& command) {
            if (command.cancel) ++cancel_intents_; else ++order_intents_;
            const bool buy = command.intent.side == oms::Side::Buy;
            Json value = { {"cancel", command.cancel}, {"monotonic_ns", command.time_ns},
                {"request_id", command.id}, {"source", command.scope.source}, {"instrument", command.intent.instrument.code},
                {"exchange", command.intent.instrument.market}, {"price", command.intent.price / double(oms::kMoneyScale)},
                {"volume", command.intent.quantity}, {"direction", std::string(1, buy ? LF_CHAR_Buy : LF_CHAR_Sell)},
                {"offset", std::string(1, buy ? LF_CHAR_Open : LF_CHAR_Close)} };
            const std::string encoded = value.dump();
            intent_crc_.process_bytes(encoded.data(), encoded.size());
        }));
        oms_ = oms::Engine::create(config, backend);
        execution_.reset(new strategy_runtime::OmsStrategyExecution(oms_, config.instance));
        const std::function<bool()> gate = [this, healthy]() {
            if (stopped_.load() || !oms_->account().ready || !healthy()) return false;
#ifdef T0_STREAM_SZE
            if (input_driver_ != "raw") return recovery_ && recovery_->live_ready();
#endif
            return true;
        };
        strategy_.reset(new MarketStrategySession(legacy, source, execution_, gate));
        if (!oms_->start_epoch(1) || !oms_->begin_reconcile(1, false))
            throw std::runtime_error("cannot initialize explicit paper account snapshot");
        snapshot.scope = oms_->scope(); snapshot.token = 1;
        snapshot.account_success = snapshot.positions_success = snapshot.orders_success = true;
        snapshot.trades_success = snapshot.all_day_orders = snapshot.all_day_trades = true;
        if (!oms_->complete_snapshot(snapshot))
            throw std::runtime_error("incomplete explicit paper account snapshot");
        strategy_->set_ready(oms_->account().ready, true, true);
    }
#ifdef T0_STREAM_SZE
    void load_recovery_config(const std::string& recording, bool capture, double day) {
        if ((input_driver_ != "sze-journal" && input_driver_ != "sze-handoff") ||
            capture != (input_driver_ == "sze-handoff"))
            throw std::runtime_error("recovery input mode mismatch");
        const Json& config = profile_.at("recovery");
        stream_input::fields(config, {"enabled", "trading_enabled", "trading_day", "source_id",
            "journal_directory", "journal_prefix", "journal_segment_mb", "journal_segment_bytes",
            "journal_max_payload_bytes", "shm_path", "expected_generation", "allow_invalid_replay_for_analysis"});
        if (!config.at("enabled").is_boolean() || !config.at("enabled").get<bool>() ||
            !config.at("trading_enabled").is_boolean() || config.at("trading_enabled").get<bool>() ||
            (config.count("allow_invalid_replay_for_analysis") &&
             (!config.at("allow_invalid_replay_for_analysis").is_boolean() ||
              config.at("allow_invalid_replay_for_analysis").get<bool>())))
            throw std::runtime_error("recovery requires strict validation and disabled trading");
        const std::uint64_t source = stream_input::uint_value(config.at("source_id"));
        const std::uint64_t trading_day = stream_input::uint_value(config.at("trading_day"));
        const std::uint64_t payload = stream_input::uint_value(config.at("journal_max_payload_bytes"));
        if (!source || source > 65535 || trading_day != static_cast<std::uint64_t>(day) ||
            payload < 72 || payload > 65535)
            throw std::runtime_error("invalid recovery day/source/payload range");
        recovery_config_.source_id = static_cast<std::uint16_t>(source);
        recovery_config_.trading_day = static_cast<std::uint32_t>(trading_day);
        recovery_config_.max_payload_bytes = static_cast<std::uint32_t>(payload);
        recovery_config_.directory = string(config, "journal_directory");
        recovery_config_.prefix = string(config, "journal_prefix");
        if (recovery_config_.prefix.empty() || absolute(recovery_config_.directory) != absolute(recording))
            throw std::runtime_error("recovery journal input path mismatch");
        if (config.count("journal_segment_mb") == config.count("journal_segment_bytes"))
            throw std::runtime_error("select exactly one recovery journal segment size");
        if (config.count("journal_segment_mb")) {
            const std::uint64_t mb = stream_input::uint_value(config.at("journal_segment_mb"));
            if (mb > std::numeric_limits<std::uint64_t>::max() / (1024 * 1024))
                throw std::runtime_error("recovery segment size overflow");
            recovery_config_.segment_bytes = mb * 1024 * 1024;
        } else recovery_config_.segment_bytes = stream_input::uint_value(config.at("journal_segment_bytes"));
        if (recovery_config_.segment_bytes < 4096 + sizeof(sze_recovery::CanonicalEvent) + 16 + payload)
            throw std::runtime_error("recovery segment cannot hold one record");
        if (config.count("expected_generation")) {
            recovery_config_.expected_generation = stream_input::uint_value(config.at("expected_generation"));
            if (!recovery_config_.expected_generation) throw std::runtime_error("recovery generation must be nonzero");
        }
        if (input_driver_ == "sze-handoff") {
            recovery_config_.shm_path = string(config, "shm_path");
            if (recovery_config_.shm_path.empty() || !recovery_config_.expected_generation)
                throw std::runtime_error("handoff requires SHM path and explicit generation");
        }
    }
#endif
    Json profile_;
    std::uint64_t rows_, predictions_, last_sequence_;
    std::uint64_t order_intents_, cancel_intents_;
    boost::crc_32_type crc_;
    boost::crc_32_type intent_crc_;
    std::string input_driver_;
    std::atomic<bool> stopped_, reached_live_;
    std::shared_ptr<oms::Engine> oms_;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> execution_;
    std::unique_ptr<MarketStrategySession> strategy_;
#ifdef T0_STREAM_SZE
    mix153060::Model model_;
    std::unique_ptr<sze_stream::SzeStreamProcessor> processor_;
    sze_stream::SzeRecoveryJournalConfig recovery_config_;
    std::unique_ptr<sze_stream::SzeRecoveryDriver> recovery_;
#else
    sse_hybrid_model::Model model_;
    sse_auction59::FactorMap auction_;
    std::unique_ptr<sse_stream::SseStreamProcessor> processor_;
#endif
};

} // market application namespace
#ifdef T0_STREAM_SZE
using sze_application::StreamProcessingCli;
#else
using sse_application::StreamProcessingCli;
#endif

#endif
