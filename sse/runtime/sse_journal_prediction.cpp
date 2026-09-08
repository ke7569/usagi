#include "sse/runtime/sse_journal_transport.h"
#include "sse/runtime/sse_cpu_affinity.h"
#include "apps/StreamProcessingCli.h"

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

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#else
    std::this_thread::yield();
#endif
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
                      std::uint64_t started_ns) {
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

    sse_cpu::Lease cpu_lease;
    std::string affinity_error;
    if (!cpu_lease.acquire(std::vector<int>(1U, config.prediction_cpu), &affinity_error) ||
        cpu_lease.cpus().size() != 1U)
        throw std::runtime_error(affinity_error.empty()
            ? "prediction CPU lease failed" : affinity_error);
    if (!sse_cpu::bind_current_thread(cpu_lease.cpus()[0].id, &affinity_error))
        throw std::runtime_error(affinity_error);

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
    application.reset(new sse_application::StreamProcessingCli(
        profile_path, true, config.journal.directory, 2U, healthy, "raw"));
    if (!consumer->open(config.journal, config.ring.path, false)) {
        std::ostringstream message;
        message << "Shanghai journal handoff open failed status="
                << static_cast<int>(consumer->last_open_status());
        throw std::runtime_error(message.str());
    }
    validate_transport_health(config, host_boot, *consumer);

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    const std::uint64_t started_ns = monotonic_ns();
    const std::uint64_t deadline_ns = add_ms(started_ns, config.duration_ms);
    std::uint64_t next_status_ns = add_ms(started_ns, 5000L);
    std::uint64_t events = 0U;
    std::uint64_t payload_bytes = 0U;
    std::uint64_t last_status_events = 0U;
    std::uint64_t last_status_ns = started_ns;
    unsigned idle_spins = 0U;
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
            try {
                // The pipeline owns its output callbacks on this same thread;
                // polling here prevents a quiet journal from delaying a ready
                // model result until the next input record.
                application->poll_outputs();
            } catch (const std::exception& exception) {
                ok = false;
                error = std::string("Shanghai journal prediction output poll failed: ") +
                        exception.what();
                break;
            } catch (...) {
                ok = false;
                error = "Shanghai journal prediction output poll failed";
                break;
            }
            if (now >= next_status_ns) {
                const std::uint64_t interval_ns = now - last_status_ns;
                const std::uint64_t delta_events = events - last_status_events;
                const std::uint64_t rate_milli = interval_ns
                    ? delta_events * 1000000000000ULL / interval_ns : 0U;
                consumer->publish_metrics(rate_milli, elapsed_ms(started_ns, now));
                std::cerr << transport_status(config, *consumer, events,
                                               payload_bytes, started_ns).dump() << '\n';
                last_status_events = events;
                last_status_ns = now;
                next_status_ns = add_ms(now, 5000L);
            }
            ++idle_spins;
            if (idle_spins >= 4096U) {
                idle_spins = 0U;
                std::this_thread::yield();
            } else {
                cpu_relax();
            }
            continue;
        }
        if (status != sze_recovery::kReplayReadEvent) {
            ok = false;
            error = status == sze_recovery::kReplayReadInvalid
                ? "Shanghai journal handoff became invalid"
                : "Shanghai journal handoff read failed";
            break;
        }

        idle_spins = 0U;

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
                                           payload_bytes, started_ns).dump() << '\n';
            last_status_events = events;
            last_status_ns = after_event;
            next_status_ns = add_ms(after_event, 5000L);
        }
    }

    bool cleanup_ok = true;
    std::string cleanup_error;
    try {
        application->begin_stop();
    } catch (const std::exception& exception) {
        cleanup_ok = false;
        cleanup_error = std::string("Shanghai journal prediction begin_stop failed: ") +
                        exception.what();
    } catch (...) {
        cleanup_ok = false;
        cleanup_error = "Shanghai journal prediction begin_stop failed";
    }
    try {
        // A pipeline implementation drains only already-closed BatchEnds;
        // it must not manufacture a close for the final open batch.
        application->finish();
    } catch (const std::exception& exception) {
        cleanup_ok = false;
        if (cleanup_error.empty())
            cleanup_error = std::string("Shanghai journal prediction finish failed: ") +
                            exception.what();
    } catch (...) {
        cleanup_ok = false;
        if (cleanup_error.empty())
            cleanup_error = "Shanghai journal prediction finish failed";
    }
    if (!cleanup_ok) {
        ok = false;
        if (error.empty()) error = cleanup_error;
    }
    if (!error.empty()) ok = false;
    const Json result = final_status(config, *consumer, application.get(),
                                     events, payload_bytes, started_ns, ok,
                                     host_boot, error);
    // close() resets the attached ring identity and replay mode. Preserve the
    // last observed handoff status before releasing those resources.
    consumer->close();
    std::cout << result.dump() << '\n';
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
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
