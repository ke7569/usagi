#include "sse/runtime/sse_journal_transport.h"
#include "sse/runtime/sse_capture_history.h"
#include "sse/runtime/sse_shm_prefetch.h"
#include "sse/runtime/sse_cpu_affinity.h"
#include "apps/StreamProcessingCli.h"
#include "common/execution/LiveTd.h"
#include "sse/market_data/sse_tick_static_metadata.h"
#include <sys/stat.h>
#include <glob.h>
#include <fcntl.h>
#include <unistd.h>

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

static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal stop flag must be lock-free");
std::atomic<int> stop_requested(0);

// Fault-only lookup: never scan journal files on the prediction hot path.
Json fault_location(const std::string& directory, std::uint64_t event_id) {
    Json result{{"event_id", event_id}, {"journal_directory", directory}};
    glob_t paths = {};
    if (::glob((directory + "/*.szej").c_str(), 0, 0, &paths) == 0) {
        for (std::size_t i = 0; i < paths.gl_pathc; ++i) {
            const int fd = ::open(paths.gl_pathv[i], O_RDONLY);
            if (fd < 0) continue;
            struct stat info = {};
            ::fstat(fd, &info);
            off_t offset = 4096;
            unsigned char header[72];
            while (::pread(fd, header, sizeof(header), offset) == sizeof(header)) {
                std::uint32_t bytes = 0;
                std::uint64_t id = 0;
                std::memcpy(&bytes, header + 8, sizeof(bytes));
                std::memcpy(&id, header + 16, sizeof(id));
                if (bytes < 88 || offset + bytes > info.st_size) break;
                if (id == event_id) {
                    result["journal_file"] = paths.gl_pathv[i];
                    result["journal_offset"] = offset;
                    break;
                }
                if (id > event_id) break;
                offset += bytes;
            }
            ::close(fd);
            if (result.count("journal_file")) break;
        }
    }
    ::globfree(&paths);
    return result;
}

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
    int strategy_cpu = -1;
    const char* strategy_cpu_text = std::getenv("SSE_STRATEGY_CPU");
    if (strategy_cpu_text && *strategy_cpu_text) {
        std::vector<int> selected;
        std::string why;
        if (!sse_cpu::parse_cpu_list(strategy_cpu_text, &selected, &why) || selected.size()!=1)
            throw std::runtime_error("SSE_STRATEGY_CPU requires one valid CPU: " + why);
        strategy_cpu = selected[0];
    }
    // Select before narrowing owner affinity. Both leases survive until the
    // application and every SDK worker have stopped.
    sse_cpu::Lease cpu_lease;
    std::string affinity_error;
    const char* shard_env=std::getenv("SSE_PREDICTION_CPUS");
    std::vector<int> shard_cpus;
    if (shard_env && *shard_env) {
        if (!sse_cpu::parse_cpu_list(shard_env,&shard_cpus,&affinity_error))
            throw std::runtime_error(affinity_error);
        requested_cpus.insert(requested_cpus.end(),shard_cpus.begin(),shard_cpus.end());
    }
    int shm_reader_cpu=-1;
    const char* reader_cpu_text=std::getenv("SSE_SHM_READER_CPU");
    if(reader_cpu_text && *reader_cpu_text) {
        std::vector<int> selected;
        if(!sse_cpu::parse_cpu_list(reader_cpu_text,&selected,&affinity_error) || selected.size()!=1)
            throw std::runtime_error("SSE_SHM_READER_CPU requires one valid CPU");
        shm_reader_cpu=selected[0];
        requested_cpus.push_back(shm_reader_cpu);
    }
    bool strategy_shares_owner_l3=false;
    if(strategy_cpu>=0) {
        if(std::find(requested_cpus.begin(),requested_cpus.end(),strategy_cpu)!=requested_cpus.end())
            throw std::runtime_error("strategy CPU must be distinct from ingress, TD and workers");
        std::vector<sse_cpu::Cpu> topology;
        if(!sse_cpu::discover(&topology,&affinity_error))throw std::runtime_error(affinity_error);
        std::string owner_domain,strategy_domain;
        for(const auto& cpu:topology) {
            if(cpu.id==requested_cpus[0])owner_domain=cpu.l3;
            if(cpu.id==strategy_cpu)strategy_domain=cpu.l3;
        }
        if(strategy_domain.empty())throw std::runtime_error("strategy CPU is unavailable");
        // Two distinct physical cores may intentionally share the ingress
        // owner's already-exclusive L3 lease; prediction workers keep theirs.
        strategy_shares_owner_l3=!owner_domain.empty() && strategy_domain==owner_domain;
        if(!strategy_shares_owner_l3)requested_cpus.push_back(strategy_cpu);
    }
    if (!cpu_lease.acquire(requested_cpus, &affinity_error) ||
        cpu_lease.cpus().size() != requested_cpus.size())
        throw std::runtime_error(affinity_error.empty()
            ? "prediction CPU lease failed" : affinity_error);
    if(!shard_cpus.empty() && profile.at("prediction").value("model_version",std::string("legacy"))=="v0.6") {
        std::vector<sse_cpu::Cpu> topology;
        if(!sse_cpu::discover(&topology,&affinity_error))throw std::runtime_error(affinity_error);
        for(int worker_cpu:shard_cpus) {
            const int model_cpu=worker_cpu+1;
            std::string worker_l3,model_l3;
            for(const auto& cpu:topology){if(cpu.id==worker_cpu)worker_l3=cpu.l3;if(cpu.id==model_cpu)model_l3=cpu.l3;}
            if(model_l3.empty() || worker_l3!=model_l3 || model_cpu==strategy_cpu ||
               std::find(requested_cpus.begin(),requested_cpus.end(),model_cpu)!=requested_cpus.end())
                throw std::runtime_error("SSE model helper must use a distinct core in its worker L3");
        }
    }
    std::unique_ptr<sze_recovery::ReplayHandoffConsumer> consumer(
        new sze_recovery::ReplayHandoffConsumer());
    std::atomic<bool> ingress_live(false);
    std::unique_ptr<sse_application::StreamProcessingCli> application;
    bool draining_transition = false;
    // Only the ingress owner accesses the mutable handoff consumer. The
    // strategy owner sees a release/acquire gate, never consumer internals.
    const auto publish_live = [&](bool value) {
        if (ingress_live.load(std::memory_order_relaxed)!=value)
            ingress_live.store(value,std::memory_order_release);
    };
    const std::function<bool()> healthy = [&]() {
        return stop_requested.load(std::memory_order_relaxed)==0 &&
            ingress_live.load(std::memory_order_acquire);
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
    std::unique_ptr<sse_journal::ShmPrefetch> prefetch;
    if(shm_reader_cpu>=0) {
        prefetch.reset(new sse_journal::ShmPrefetch(consumer->live_ring(),shm_reader_cpu));
        consumer->set_ring_reader([&](std::uint64_t expected,sze_recovery::CanonicalEvent* event,const unsigned char** bytes) {
            const sse_journal::ShmPrefetch::Record* record=nullptr;
            const auto status=prefetch->read(expected,&record);
            if(status==sze_recovery::kRingReadOk){*event=record->event;*bytes=record->payload.data();}
            return status;
        });
    }
    const int td_cpu = native_td ? cpu_lease.cpus()[1].id : -1;
    if (td_cpu >= 255) throw std::runtime_error("leased TD CPU exceeds ATP's supported CPU range");
    if(native_td && setenv("SSE_LEASED_TD_CPU",std::to_string(td_cpu).c_str(),1))
        throw std::runtime_error("cannot pass leased TD CPU");
    // First-touch model/owner queues on the prediction node. Only SDK
    // initialization temporarily switches to the leased callback CPU.
    if (!sse_cpu::bind_current_thread(cpu_lease.cpus()[0].id, &affinity_error))
        throw std::runtime_error(affinity_error);
    application.reset(new sse_application::StreamProcessingCli(
        profile_path, true, config.journal.directory, 2U, healthy, "raw", true));
    if (!sse_cpu::bind_current_thread(cpu_lease.cpus()[0].id, &affinity_error))
        throw std::runtime_error(affinity_error);
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    sse_journal::OverlapFilter active_overlap;
    bool chained_history=false;
    const char* chain_path=std::getenv("SSE_HISTORY_CHAIN");
    const char* chain_day=std::getenv("SSE_HISTORY_CHAIN_DAY");
    if(chain_path && *chain_path && chain_day && std::to_string(config.journal.trading_day)==chain_day) {
        const Json chain=load_stream_json(chain_path);
        if(chain.at("trading_day").get<std::uint32_t>()==config.journal.trading_day) {
            if(chain.at("active_generation").get<std::uint64_t>()!=config.journal.generation)
                throw std::runtime_error("capture history active generation mismatch");
            chained_history=true;
            std::set<std::uint64_t> generations;generations.insert(config.journal.generation);
            for(const auto& segment:chain.at("segments")) {
                const auto history=sse_journal::load(segment.at("capture_config").get<std::string>(),true);
                if(history.journal.trading_day!=config.journal.trading_day || history.journal.source_id!=config.journal.source_id ||
                   history.boot!=config.boot || !generations.insert(history.journal.generation).second || history.channels.size()!=config.channels.size())
                    throw std::runtime_error("capture history identity mismatch");
                for(std::size_t c=0;c<config.channels.size();++c)
                    if(history.channels[c].group!=config.channels[c].group || history.channels[c].port!=config.channels[c].port || history.channels[c].interface_ip!=config.channels[c].interface_ip)
                        throw std::runtime_error("capture history subscription mismatch");
                const auto limit=segment.at("last_event_id").get<std::uint64_t>();
                if(!limit)throw std::runtime_error("empty capture history segment");
                sse_journal::OverlapFilter filter;
                if(segment.count("overlap"))filter.configure(segment.at("overlap"),config.channels.size());
                sze_recovery::JournalReader reader;
                if(reader.open(history.journal).status!=sze_recovery::kJournalOk)throw std::runtime_error("capture history journal open failed");
                std::vector<unsigned char> bytes(history.journal.max_payload_bytes);sze_recovery::CanonicalEvent record={};
                std::uint64_t accepted=0,last_mono=0;
                for(std::uint64_t n=1;n<=limit;++n) {
                    if(stop_requested)throw std::runtime_error("capture history replay interrupted");
                    if(reader.next(&record,bytes.data(),bytes.size())!=sze_recovery::kJournalOk || record.event_id!=n)
                        throw std::runtime_error("capture history prefix incomplete or corrupt");
                    const auto e=sse_journal::decode(record,bytes.data(),history);
                    if(filter.accept(e,n)){application->on_event(e);last_mono=e.monotonic_ns;++accepted;}
                    if(n%1000000==0)std::cerr<<Json{{"event","capture_history_replay"},{"generation",history.journal.generation},{"events",n},{"accepted",accepted}}.dump()<<'\n';
                }
                filter.complete();
                if(accepted)application->begin_transport_epoch(last_mono);
                std::cerr<<Json{{"event","capture_history_segment_complete"},{"generation",history.journal.generation},{"events",limit},{"accepted",accepted}}.dump()<<'\n';
            }
            active_overlap.configure(chain.at("active_overlap"),config.channels.size());
        }
    }
    const char* history_path=std::getenv("SSE_HISTORY_CAPTURE");
    const char* history_day=std::getenv("SSE_HISTORY_DAY");
    if(!chained_history && history_path && *history_path && history_day && std::to_string(config.journal.trading_day)==history_day) {
        const auto history=sse_journal::load(history_path,true);
        if(history.journal.trading_day!=config.journal.trading_day || history.journal.source_id!=config.journal.source_id || history.journal.generation==config.journal.generation)
            throw std::runtime_error("invalid prior capture replay boundary");
        sze_recovery::JournalReader prior;
        if(prior.open(history.journal).status!=sze_recovery::kJournalOk)throw std::runtime_error("cannot open prior capture journal");
        std::vector<unsigned char> bytes(history.journal.max_payload_bytes);sze_recovery::CanonicalEvent record={};
        std::uint64_t count=0,last_mono=0;
        const char* limit_text=std::getenv("SSE_HISTORY_LAST_EVENT");
        const std::uint64_t history_limit=limit_text?std::stoull(limit_text):0;
        for(;;){if(history_limit && count==history_limit)break;const auto result=prior.next(&record,bytes.data(),bytes.size());if(result==sze_recovery::kJournalWouldBlock || result==sze_recovery::kJournalEnd)break;
            if(result!=sze_recovery::kJournalOk)throw std::runtime_error("prior journal corruption");
            const auto event=sse_journal::decode(record,bytes.data(),history);application->on_event(event);last_mono=event.monotonic_ns;++count;
            if(count%1000000==0)std::cerr<<Json{{"event","prior_capture_replay"},{"events",count}}.dump()<<'\n';
        }
        if(!count || (history_limit && count!=history_limit))throw std::runtime_error("prior journal prefix incomplete");
        application->begin_transport_epoch(last_mono);
        std::cerr<<Json{{"event","prior_capture_replay_complete"},{"events",count}}.dump()<<'\n';
    }
    std::cerr << Json{{"event", "journal_prediction_cpu"},
        {"prediction_cpu", cpu_lease.cpus()[0].id}, {"td_cpu", td_cpu}, {"shm_reader_cpu", shm_reader_cpu},
        {"prediction_worker_cpus",shard_cpus},{"strategy_cpu",strategy_cpu},{"strategy_shares_owner_l3",strategy_shares_owner_l3}}.dump() << '\n';

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
        sze_recovery::CanonicalEvent canonical = {};
        const auto previous_mode = consumer->mode();
        const unsigned char* input_payload=payload.data();
        const sze_recovery::ReplayReadStatus status = consumer->next(
            &canonical, payload.data(), payload.size(), &input_payload);
        if (consumer->mode() != previous_mode) {
            // Deliver every buffered historical output while the execution
            // gate is closed, before admitting the first live event.
            publish_live(false);
            draining_transition = true;
            try { active_overlap.complete();application->flush_processing(); }
            catch (const std::exception& e) { ok=false;error=e.what();break; }
            draining_transition = false;
            application->enable_live_latency(consumer->mode()==sze_recovery::kReplayLive);
        }
        // next() has validated producer identity/aliveness and continuity.
        // A live transition is published only after every historical strategy
        // message has drained above, including its OMS work.
        publish_live(!draining_transition && consumer->mode()==sze_recovery::kReplayLive &&
            (status==sze_recovery::kReplayReadEvent || status==sze_recovery::kReplayReadWouldBlock) &&
            application->processing_valid());
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
            // Dedicated prediction/strategy owner core. No 100us idle sleep:
            // poll the journal and TD again as soon as this iteration ends.
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#else
            std::this_thread::yield();
#endif
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
                canonical, input_payload, config);
            if(active_overlap.accept(event,canonical.event_id))application->on_event(event);
            ++events;
            payload_bytes += event.size;
        } catch (const std::exception& exception) {
            ok = false;
            publish_live(false);
            error = std::string("Shanghai journal prediction consumer failed: ") +
                    exception.what();
            Json fault = fault_location(config.journal.directory, canonical.event_id);
            fault["event"] = "journal_prediction_fault";
            fault["error"] = error;
            std::cerr << fault.dump() << '\n';
            break;
        } catch (...) {
            ok = false;
            error = "Shanghai journal prediction consumer failed";
            break;
        }

        const std::uint64_t after_event = monotonic_ns();
        if (after_event >= next_status_ns) {
            application->poll();
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

    publish_live(false);
    if (ok) {
        try { application->flush_processing(); }
        catch (const std::exception& e) { ok=false;error=e.what(); }
    }
    if (!error.empty()) ok = false;
    // Join delivery even on a processing fault before reading final statistics.
    // Otherwise a failed queue could throw again during summary and turn a
    // deterministic gap (exit 65) into an automatic restart loop.
    try { application->begin_stop(); }
    catch(const std::exception& e) { ok=false;if(error.empty())error=e.what(); }
    const Json result = final_status(config, *consumer, application.get(),
                                     events, payload_bytes, started_ns, ok,
                                     host_boot, error);
    if(prefetch)prefetch->stop();
    consumer->close();
    std::cout << result.dump() << '\n';
    // A deterministic data fault must not replay the same gap every 30 seconds.
    if (!ok && error.find("SSE tick sequence gap") != std::string::npos) return 65;
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
        const Json daily = load_stream_json(daily_path);
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
        Json holdings = Json::array();
        for (const auto& instrument : universe) {
            oms::Position position;
            if (runtime.engine->position(instrument, &position)) {
                ++positions;
                if (position.total || position.sellable) ++nonzero;
                if (position.total || position.sellable ||
                    daily.at("ins_params").at(instrument.code + ".SH").value("static_position", 0LL) > 0)
                    holdings.push_back(Json{{"instrument", instrument.code},
                        {"total", position.total}, {"sellable", position.sellable},
                        {"working_buy", position.working_buy}, {"working_sell", position.working_sell}});
            }
        }
        result["connected"] = account.connected;
        result["ready"] = account.ready && !stop_requested;
        result["positions"] = positions;
        result["nonzero_positions"] = nonzero;
        result["positions_scope"] = "daily_universe";
        result["holdings"] = holdings;
        result["available_cash"] = double(account.available_cash) / oms::kMoneyScale;
        result["cash_reserved"] = double(account.cash_reserved) / oms::kMoneyScale;
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
