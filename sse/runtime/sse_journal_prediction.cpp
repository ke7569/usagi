#include "sse/runtime/sse_journal_transport.h"
#include "sse/runtime/sse_cpu_affinity.h"
#include "apps/StreamProcessingCli.h"
#include "common/execution/LiveTd.h"
#include "sse/market_data/sse_tick_static_metadata.h"
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

typedef nlohmann::json Json;
typedef std::chrono::steady_clock Clock;

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) {
    stop_requested = 1;
}

std::uint64_t monotonic_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

std::string mode_name(sze_recovery::ReplayMode mode) {
    switch (mode) {
    case sze_recovery::kReplayJournal: return "replay";
    case sze_recovery::kReplayHandoff: return "handoff";
    case sze_recovery::kReplayLive: return "live";
    case sze_recovery::kReplayInvalid: return "invalid";
    }
    return "invalid";
}

std::uint64_t elapsed_ms(std::uint64_t begin, std::uint64_t end) {
    return end >= begin ? (end - begin) / 1000000ULL : 0ULL;
}

std::uint64_t add_ms(std::uint64_t value, long milliseconds) {
    if (milliseconds <= 0) return value;
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    if (static_cast<std::uint64_t>(milliseconds) > maximum / 1000000ULL)
        return maximum;
    const std::uint64_t delta = static_cast<std::uint64_t>(milliseconds) * 1000000ULL;
    return delta > maximum - value ? maximum : value + delta;
}

long parse_duration(const char* text) {
    if (!text || !*text) throw std::runtime_error("duration must be a nonnegative integer");
    char* end = 0;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno || end == text || *end ||
        value > static_cast<unsigned long long>((std::numeric_limits<long>::max)()))
        throw std::runtime_error("duration must be a nonnegative integer");
    return static_cast<long>(value);
}

void validate_transport_health(const sse_journal::Config& config,
                               const std::string& host_boot,
                               const sze_recovery::ReplayHandoffConsumer& consumer) {
    if (config.boot != host_boot)
        throw std::runtime_error("journal transport boot identity changed");
    if (!consumer.producer_alive())
        throw std::runtime_error("Shanghai journal producer is not alive");
    if (consumer.ring_trading_day() != config.journal.trading_day ||
        consumer.ring_source_id() != config.journal.source_id ||
        consumer.generation() != config.journal.generation ||
        consumer.ring_generation() != config.journal.generation)
        throw std::runtime_error("Shanghai journal transport day/source/generation mismatch");
    if (consumer.continuity_state() == sze_recovery::kContinuityInvalid)
        throw std::runtime_error("Shanghai journal transport continuity is invalid");
}

Json transport_status(const sse_journal::Config& config,
                      const sze_recovery::ReplayHandoffConsumer& consumer,
                      std::uint64_t events, std::uint64_t payload_bytes,
                      std::uint64_t started_ns,
                      const sse_application::StreamProcessingCli* application = 0) {
    Json result;
    result["event"] = "journal_prediction_status";
    result["mode"] = mode_name(consumer.mode());
    result["events"] = events;
    result["payload_bytes"] = payload_bytes;
    result["replayed_events"] = consumer.replayed_events();
    result["live_events"] = consumer.live_events();
    result["replay_lag"] = consumer.replay_lag();
    result["handoff_retries"] = consumer.handoff_retries();
    result["ring_overruns"] = consumer.ring_overruns();
    result["generation"] = consumer.generation();
    result["ring_generation"] = consumer.ring_generation();
    result["trading_day"] = config.journal.trading_day;
    result["source_id"] = config.journal.source_id;
    result["boot_id"] = config.boot;
    result["producer_alive"] = consumer.producer_alive();
    result["continuity_valid"] =
        consumer.continuity_state() != sze_recovery::kContinuityInvalid;
    result["elapsed_ms"] = elapsed_ms(started_ns, monotonic_ns());
    if (application) result["processing"] = application->runtime_status();
    return result;
}

Json final_status(const sse_journal::Config& config,
                  const sze_recovery::ReplayHandoffConsumer& consumer,
                  const sse_application::StreamProcessingCli* application,
                  std::uint64_t events, std::uint64_t payload_bytes,
                  std::uint64_t started_ns, bool ok,
                  const std::string& host_boot,
                  const std::string& error) {
    Json result;
    result["ok"] = ok;
    result["mode"] = mode_name(consumer.mode());
    result["events"] = events;
    result["payload_bytes"] = payload_bytes;
    result["replayed_events"] = consumer.replayed_events();
    result["live_events"] = consumer.live_events();
    result["replay_lag"] = consumer.replay_lag();
    result["handoff_retries"] = consumer.handoff_retries();
    result["ring_overruns"] = consumer.ring_overruns();
    result["generation"] = consumer.generation();
    result["ring_generation"] = consumer.ring_generation();
    result["trading_day"] = config.journal.trading_day;
    result["source_id"] = config.journal.source_id;
    result["boot_id"] = config.boot;
    result["producer_alive"] = consumer.producer_alive();
    result["health"] = Json{
        {"producer_alive", consumer.producer_alive()},
        {"trading_day", consumer.ring_trading_day() == config.journal.trading_day},
        {"source_id", consumer.ring_source_id() == config.journal.source_id},
        {"generation", consumer.generation() == config.journal.generation &&
                        consumer.ring_generation() == config.journal.generation},
        {"boot_id", config.boot == host_boot},
        {"continuity_valid", consumer.continuity_state() != sze_recovery::kContinuityInvalid}
    };
    result["elapsed_ms"] = elapsed_ms(started_ns, monotonic_ns());
    if (application) {
        result["processing"] = application->summary();
        result["processing_valid"] = application->processing_valid();
    }
    if (!error.empty()) result["error"] = error;
    return result;
}

int run(const std::string& config_path, const std::string& profile_path,
        long duration_override, bool has_duration_override) {
    sse_journal::Config config = sse_journal::load(config_path);
    if (has_duration_override) config.duration_ms = duration_override;
    if (config.channels.empty()) throw std::runtime_error("journal prediction requires channels");
    if (config.prediction_cpu < -1)
        throw std::runtime_error("invalid prediction CPU");
    // Config::load validates this once. Keep the value for health reporting so
    // the live handoff loop never performs filesystem I/O for boot identity.
    const std::string host_boot = sse_journal::boot_id();
    if (config.boot != host_boot)
        throw std::runtime_error("journal transport boot identity changed");

    const Json profile = load_stream_json(profile_path);
    const std::string execution = profile.value("execution", std::string("disabled"));
    const bool native_td = execution == "live" || execution == "monitor";
    std::vector<int> requested_cpus(1U, config.prediction_cpu);
    if (native_td) {
        int td_cpu = -1;
        const Json& td = profile.at("strategy_runtime").at("td");
        if (td.count("cpu")) {
            const Json& value = td.at("cpu");
            if (!value.is_number_integer() || value.get<long long>() < -1 || value.get<long long>() >= 255)
                throw std::runtime_error("TD CPU must be -1 or an ATP-supported CPU in [0,254]");
            td_cpu = value.get<int>();
        }
        requested_cpus.push_back(td_cpu);
    }
    // Select before narrowing owner affinity. Both leases survive until the
    // application and every SDK worker have stopped.
    sse_cpu::Lease cpu_lease;
    std::string affinity_error;
    if (!cpu_lease.acquire(requested_cpus, &affinity_error) ||
        cpu_lease.cpus().size() != requested_cpus.size())
        throw std::runtime_error(affinity_error.empty()
            ? "prediction CPU lease failed" : affinity_error);
    std::unique_ptr<sze_recovery::ReplayHandoffConsumer> consumer(
        new sze_recovery::ReplayHandoffConsumer());
    std::unique_ptr<sse_application::StreamProcessingCli> application;
    const std::function<bool()> healthy = [&]() {
        return stop_requested == 0 && consumer->mode() == sze_recovery::kReplayLive &&
            consumer->producer_alive() &&
            consumer->ring_trading_day() == config.journal.trading_day &&
            consumer->ring_source_id() == config.journal.source_id &&
            consumer->generation() == config.journal.generation &&
            consumer->ring_generation() == config.journal.generation &&
            (!application || application->processing_valid());
    };

    // The prediction process consumes source-89 records as a live application
    // stream. It does not create a MarketDataStream and never writes capture
    // files; journal and SHM ownership stays with the producer.
    if (!consumer->open(config.journal, config.ring.path, false)) {
        std::ostringstream message;
        message << "Shanghai journal handoff open failed status="
                << static_cast<int>(consumer->last_open_status());
        throw std::runtime_error(message.str());
    }
    validate_transport_health(config, host_boot, *consumer);
    const int td_cpu = native_td ? cpu_lease.cpus()[1].id : -1;
    if (td_cpu >= 255) throw std::runtime_error("leased TD CPU exceeds ATP's supported CPU range");
    if (!sse_cpu::bind_current_thread(native_td ? td_cpu : cpu_lease.cpus()[0].id, &affinity_error))
        throw std::runtime_error(affinity_error);
    application.reset(new sse_application::StreamProcessingCli(
        profile_path, true, config.journal.directory, 2U, healthy, "raw", true));
    if (!sse_cpu::bind_current_thread(cpu_lease.cpus()[0].id, &affinity_error))
        throw std::runtime_error(affinity_error);
    std::cerr << Json{{"event", "journal_prediction_cpu"},
        {"prediction_cpu", cpu_lease.cpus()[0].id}, {"td_cpu", td_cpu}}.dump() << '\n';

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    const std::uint64_t started_ns = monotonic_ns();
    const std::uint64_t deadline_ns = add_ms(started_ns, config.duration_ms);
    std::uint64_t next_status_ns = add_ms(started_ns, 5000L);
    std::uint64_t events = 0U;
    std::uint64_t payload_bytes = 0U;
    std::uint64_t last_status_events = 0U;
    std::uint64_t last_status_ns = started_ns;
    bool ok = true;
    std::string error;
    std::vector<unsigned char> payload(config.journal.max_payload_bytes, 0U);

    while (!stop_requested) {
        const std::uint64_t now = monotonic_ns();
        if (config.duration_ms > 0 && now >= deadline_ns) break;
        try {
            validate_transport_health(config, host_boot, *consumer);
        } catch (const std::exception& exception) {
            ok = false;
            error = exception.what();
            break;
        }

        sze_recovery::CanonicalEvent canonical = {};
        const sze_recovery::ReplayReadStatus status = consumer->next(
            &canonical, payload.data(), payload.size());
        if (status == sze_recovery::kReplayReadWouldBlock) {
            try { application->poll(); }
            catch (const std::exception& exception) {
                ok = false;
                error = std::string("Shanghai TD idle poll failed: ") + exception.what();
                break;
            }
            if (now >= next_status_ns) {
                const std::uint64_t interval_ns = now - last_status_ns;
                const std::uint64_t delta_events = events - last_status_events;
                const std::uint64_t rate_milli = interval_ns
                    ? delta_events * 1000000000000ULL / interval_ns : 0U;
                consumer->publish_metrics(rate_milli, elapsed_ms(started_ns, now));
                std::cerr << transport_status(config, *consumer, events,
                                               payload_bytes, started_ns, application.get()).dump() << '\n';
                last_status_events = events;
                last_status_ns = now;
                next_status_ns = add_ms(now, 5000L);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        if (status != sze_recovery::kReplayReadEvent) {
            ok = false;
            error = status == sze_recovery::kReplayReadInvalid
                ? "Shanghai journal handoff became invalid"
                : "Shanghai journal handoff read failed";
            break;
        }

        try {
            deepwin_market_data::StreamEvent event = sse_journal::decode(
                canonical, payload.data(), config);
            application->on_event(event);
            ++events;
            payload_bytes += event.size;
        } catch (const std::exception& exception) {
            ok = false;
            error = std::string("Shanghai journal prediction consumer failed: ") +
                    exception.what();
            break;
        } catch (...) {
            ok = false;
            error = "Shanghai journal prediction consumer failed";
            break;
        }

        const std::uint64_t after_event = monotonic_ns();
        if (after_event >= next_status_ns) {
            const std::uint64_t interval_ns = after_event - last_status_ns;
            const std::uint64_t delta_events = events - last_status_events;
            const std::uint64_t rate_milli = interval_ns
                ? delta_events * 1000000000000ULL / interval_ns : 0U;
            consumer->publish_metrics(rate_milli, elapsed_ms(started_ns, after_event));
            std::cerr << transport_status(config, *consumer, events,
                                           payload_bytes, started_ns, application.get()).dump() << '\n';
            last_status_events = events;
            last_status_ns = after_event;
            next_status_ns = add_ms(after_event, 5000L);
        }
    }

    if (!error.empty()) ok = false;
    const Json result = final_status(config, *consumer, application.get(),
                                     events, payload_bytes, started_ns, ok,
                                     host_boot, error);
    application->begin_stop();
    consumer->close();
    std::cout << result.dump() << '\n';
    return ok ? 0 : 1;
}


std::string query_text(const Json& config, const char* key, bool absolute = false) {
    const Json::const_iterator value = config.find(key);
    if (value == config.end() || !value->is_string() || value->get<std::string>().empty())
        throw std::runtime_error(std::string("TD query configuration requires ") + key);
    const std::string result = value->get<std::string>();
    if (absolute && result[0] != '/')
        throw std::runtime_error(std::string("TD query configuration requires absolute ") + key);
    return result;
}

struct QueryTdRuntime {
    // The engine owns a plugin backend. Stop callbacks and release it before dlclose.
    std::unique_ptr<strategy_runtime::LiveTdPlugin> plugin;
    std::shared_ptr<oms::Engine> engine;
    ~QueryTdRuntime() {
        try { if (engine) engine->begin_stop(); } catch (...) {}
        try { if (plugin) plugin->session().stop(); } catch (...) {}
        try { if (plugin) plugin->session().attach(std::shared_ptr<oms::Engine>()); } catch (...) {}
        engine.reset();
    }
};

int run_td_query_only(const std::string& public_path) {
    Json result{{"event", "td_query_only"}, {"orders_enabled", false},
                {"connected", false}, {"ready", false}, {"positions", 0},
                {"orders", 0}, {"trades", 0}};
    const std::uint64_t started_ns = monotonic_ns();
    QueryTdRuntime runtime;
    try {
        const Json input = load_stream_json(public_path);
        if (!input.is_object()) throw std::runtime_error("TD query configuration must be an object");
        const std::string library = query_text(input, "library", true);
        const std::string private_path = query_text(input, "config_path", true);
        const std::string daily_path = query_text(input, "daily_config", true);
        const std::string journal_path = query_text(input, "journal_path", true);
        const Json::const_iterator date = input.find("trading_day");
        if (date == input.end() || !date->is_number_integer())
            throw std::runtime_error("TD query trading_day must be an integer");
        const long long trading_day = date->get<long long>();
        if (trading_day < 20000101 || trading_day > 20991231)
            throw std::runtime_error("TD query trading_day is invalid");
        std::uint64_t epoch = monotonic_ns();
        if (!epoch) epoch = 1;
        if (input.count("epoch")) {
            const Json& value = input.at("epoch");
            if (!value.is_number_integer() ||
                (!value.is_number_unsigned() && value.get<long long>() <= 0) ||
                (value.is_number_unsigned() && value.get<std::uint64_t>() == 0))
                throw std::runtime_error("TD query epoch must be a positive uint64 integer");
            epoch = value.get<std::uint64_t>();
        }
        long timeout_ms = 20000L;
        const Json::const_iterator timeout = input.find("timeout_ms");
        if (timeout != input.end()) {
            if (!timeout->is_number_integer())
                throw std::runtime_error("TD query timeout_ms must be an integer");
            const long long configured = timeout->get<long long>();
            if (configured < 1 || configured > 60000)
                throw std::runtime_error("TD query timeout_ms must be in [1,60000]");
            timeout_ms = static_cast<long>(configured);
        }
        struct stat secret_stat;
        if (::stat(private_path.c_str(), &secret_stat) || !S_ISREG(secret_stat.st_mode) ||
            secret_stat.st_uid != 0 || (secret_stat.st_mode & 0077))
            throw std::runtime_error("TD private configuration must be a root-owned private regular file");
        oms::Scope scope;
        scope.account.broker = query_text(input, "broker");
        scope.account.account = query_text(input, "account");
        scope.gateway = "sse_td";
        scope.source = 190;
        scope.day = static_cast<std::uint32_t>(trading_day);
        scope.epoch = epoch;
        sse_tick::DailyStaticMetadataMap metadata;
        std::string error;
        if (!sse_tick::load_daily_static_metadata_json(daily_path, scope.day, &metadata, &error))
            throw std::runtime_error(error);
        if (metadata.empty()) throw std::runtime_error("TD query daily universe is empty");
        oms::Config config;
        config.scope = scope;
        config.scope.epoch = 0;
        config.instance = "sse_td_query_only";
        config.ownership = oms::OwnershipMode::ExclusiveLocal;
        config.lock_directory = "/run/usagi/oms/accounts";
        config.enabled = true;  // Readiness means reconciliation, never order permission.
        config.restart_cancel_open_orders = false;
        config.journal_path = journal_path;
        std::set<oms::Instrument> universe;
        for (const auto& row : metadata) {
            const oms::Instrument instrument = {"SSE", row.first};
            oms::InstrumentRules rules;
            if (!row.second.has_limit_price || !row.second.has_stop_price ||
                !oms::money_from_double(row.second.stop_price, &rules.lower_price) ||
                !oms::money_from_double(row.second.limit_price, &rules.upper_price) ||
                rules.lower_price <= 0 || rules.upper_price < rules.lower_price)
                throw std::runtime_error("TD query daily price bounds are invalid");
            universe.insert(instrument);
            config.instruments[instrument] = rules;
        }
        result["universe_size"] = universe.size();
        stop_requested = 0;
        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
        try {
            runtime.plugin.reset(new strategy_runtime::LiveTdPlugin(
                library, private_path, scope, universe, false));
        } catch (...) {
            // A vendor exception can contain fragments of the private JSON.
            throw std::runtime_error("live TD plugin initialization failed");
        }
        runtime.engine = oms::Engine::create(config, runtime.plugin->session().backend());
        runtime.plugin->session().attach(runtime.engine);
        if (!runtime.engine->start_epoch(scope.epoch, false))
            throw std::runtime_error("OMS refused the TD connection epoch");
        const std::uint64_t deadline_ns = add_ms(monotonic_ns(), timeout_ms);
        if (stop_requested) throw std::runtime_error("interrupted");
        if (!runtime.plugin->session().connect(timeout_ms * 1000000L, &error))
            throw std::runtime_error(error.empty() ? "TD login failed" : error);
        runtime.engine->advance_to(static_cast<oms::Time>(monotonic_ns()));
        if (!runtime.engine->set_connected(scope, true))
            throw std::runtime_error("OMS refused the TD connection scope");
        result["connected"] = true;
        if (stop_requested) throw std::runtime_error("interrupted");
        if (!runtime.engine->begin_reconcile(monotonic_ns()))
            throw std::runtime_error("OMS refused complete account reconciliation");
        oms::AccountView account;
        while (!stop_requested) {
            runtime.engine->advance_to(static_cast<oms::Time>(monotonic_ns()));
            account = runtime.engine->account();
            if (account.ready || !account.connected || monotonic_ns() >= deadline_ns) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        account = runtime.engine->account();
        std::size_t positions = 0, nonzero = 0;
        for (const auto& instrument : universe) {
            oms::Position position;
            if (runtime.engine->position(instrument, &position)) {
                ++positions;
                if (position.total || position.sellable) ++nonzero;
            }
        }
        result["connected"] = account.connected;
        result["ready"] = account.ready && !stop_requested;
        result["positions"] = positions;
        result["nonzero_positions"] = nonzero;
        result["positions_scope"] = "daily_universe";
        result["orders"] = account.orders;
        result["trades"] = account.trade_ids;
        result["reason"] = stop_requested ? "interrupted" :
            account.ready ? "ready" : account.reason;
        if (!account.ready) {
            result["td_status"] = runtime.plugin->session().status();
            result["timeout"] = !stop_requested && monotonic_ns() >= deadline_ns;
        }
    } catch (const std::exception& error) {
        result["reason"] = error.what();
    }
    result["elapsed_ms"] = elapsed_ms(started_ns, monotonic_ns());
    result["ok"] = result["ready"];
    std::cout << result.dump() << '\n';
    return result["ready"].get<bool>() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--td-query-only") {
        if (argc != 3) {
            std::cerr << "usage: t0_sse_journal_predict --td-query-only PUBLIC_CONFIG.json\n";
            return 2;
        }
        return run_td_query_only(argv[2]);
    }
    if (argc != 3 && argc != 5) {
        std::cerr << "usage: t0_sse_journal_predict CONFIG.json PROFILE.json"
                     " [--duration-ms N]\n";
        return 2;
    }
    if (argc == 5 && std::string(argv[3]) != "--duration-ms") {
        std::cerr << "usage: t0_sse_journal_predict CONFIG.json PROFILE.json"
                     " [--duration-ms N]\n";
        return 2;
    }
    try {
        const bool has_duration_override = argc == 5;
        const long duration = has_duration_override ? parse_duration(argv[4]) : 0L;
        return run(argv[1], argv[2], duration, has_duration_override);
    } catch (const std::exception& error) {
        std::cerr << "t0_sse_journal_predict: " << error.what() << '\n';
        return 2;
    }
}
