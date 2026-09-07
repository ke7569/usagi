#include "sse/runtime/sse_cpu_affinity.h"
#include "../market_data/sse_event_codec.h"
#include "../market_data/sse_event_journal.h"
#include "../market_data/sse_event_pipeline.h"
#include "../market_data/sse_primary_decoder.h"
#include "../market_data/sse_tick_order_book.h"
#include "../market_data/sse_history_input.h"
#include "../strategy/sse_model_consumer.h"
#include "../udp_runtime/UdpChannelRuntime.h"
#include "third_party/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <cmath>

namespace {
using nlohmann::json;
using namespace sse_pipeline;
volatile std::sig_atomic_t stop_requested = 0;
void stop_receive(int) { stop_requested = 1; }

struct Stock {
    explicit Stock(const std::string& symbol) : book(symbol), ticks(0), snapshots(0), rejected(0), channel(0) {}
    sse_tick::OrderBook book;
    std::uint64_t ticks, snapshots, rejected;
    std::uint32_t channel;
};

struct BookConsumer {
    std::map<std::string, std::unique_ptr<Stock> > stocks;
    bool operator()(const Event& event) {
        const std::string symbol(event.security_id);
        if (!stocks.count(symbol)) stocks[symbol].reset(new Stock(symbol));
        Stock& stock = *stocks[symbol];
        stock.channel = event.channel_no;
        if (event.kind == kSnapshot) { ++stock.snapshots; return true; }
        sse_live::TickEvent tick;
        if (!sse_live::decode_primary_tick(event.payload, event.payload_size, &tick)) return false;
        const sse_tick::ApplyResult result = stock.book.apply(tick);
        ++stock.ticks;
        if (!result.accepted || !result.sequence_healthy) ++stock.rejected;
        // This first consumer validates book ownership; model-window state is
        // not used yet and must not grow for the lifetime of a capture.
        if (stock.ticks % 256 == 0) stock.book.take_flow_window();
        return result.accepted && result.sequence_healthy;
    }
};

std::size_t positive(const json& config, const char* key, std::size_t fallback) {
    if (!config.count(key)) return fallback;
    const json& value = config[key];
    if (!value.is_number_integer() || value <= 0 || value > 1048576)
        throw std::runtime_error(std::string("invalid positive size: ") + key);
    return value.get<std::size_t>();
}

int cpu_value(const json& value) {
    if (!value.is_number_integer() || value < -1 || value > 1023)
        throw std::runtime_error("invalid SSE CPU index");
    return value.get<int>();
}

void write_json(const std::string& path, const json& value) {
    std::ofstream file(path.c_str());
    if (!file || !(file << value.dump(2) << '\n')) throw std::runtime_error("cannot write " + path);
}

json latency_json(const LatencyHistogram& h) {
    return {{"count",h.count()},{"min_ns",h.min_ns()},{"mean_ns",h.mean_ns()},
        {"p50_ns",h.percentile_ns(50)},{"p95_ns",h.percentile_ns(95)},
        {"p99_ns",h.percentile_ns(99)},{"p999_ns",h.percentile_ns(99.9)},{"max_ns",h.max_ns()}};
}

ModelOptions model_options(const json& config,std::uint32_t day) {
    ModelOptions result;
    result.trading_day=day;
    result.tick_model=config.at("tick_model").get<std::string>();
    result.static_json=config.at("static_json").get<std::string>();
    result.tick_start_us=config.value("tick_start_us",result.tick_start_us);
    result.snapshot_enabled=config.value("snapshot_enabled",false);
    if (result.snapshot_enabled) {
        result.snapshot_baseline=config.at("snapshot_baseline").get<std::string>();
        result.snapshot_baseline_scaler=config.at("snapshot_baseline_scaler").get<std::string>();
        result.snapshot_auction=config.at("snapshot_auction").get<std::string>();
        result.snapshot_auction_scaler=config.at("snapshot_auction_scaler").get<std::string>();
        result.auction_csv=config.value("auction_csv",std::string());
    }
    return result;
}

json summarize(const PipelineStats& stats) {
    json result;
    result["submitted"] = stats.submitted;
    result["events"] = stats.events;
    result["duplicates"] = stats.duplicates;
    result["late_ticks"] = stats.late_ticks;
    result["ingress_overflow"] = stats.ingress_overflow;
    result["journal_written"] = stats.journal_written;
    result["journal_overflow"] = stats.journal_overflow;
    result["journal_errors"] = stats.journal_errors;
    result["pending_snapshots"] = stats.pending_snapshots;
    result["pending_overflow"] = stats.pending_overflow;
    result["routing_errors"] = stats.routing_errors;
    result["shard_consumed"] = stats.shard_consumed;
    result["shard_overflow"] = stats.shard_overflow;
    result["shard_errors"] = stats.shard_errors;
    result["channel_stock_counts"] = stats.channel_stock_counts;
    result["ingress_high_water"] = stats.ingress_high_water;
    result["shard_high_water"] = stats.shard_high_water;
    result["journal_high_water"] = stats.journal_high_water;
    result["compute_healthy"] = stats.compute_healthy;
    result["journal_healthy"] = stats.journal_healthy;
    result["ingress_latency"]=latency_json(stats.ingress_latency);
    json services=json::array(), ends=json::array(), samples=json::array();
    for (const auto& h : stats.shard_service_latency) services.push_back(latency_json(h));
    for (const auto& h : stats.shard_end_to_end_latency) ends.push_back(latency_json(h));
    for (const auto& h : stats.shard_sample_end_to_end_latency) samples.push_back(latency_json(h));
    result["shard_service_latency"]=services;
    result["shard_end_to_end_latency"]=ends;
    result["shard_sample_end_to_end_latency"]=samples;
    return result;
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--replay" && std::string(argv[2]) != "--benchmark")) {
        std::cerr << "usage: sse_event_pipeline CONFIG.json [--replay|--benchmark]\n";
        return 2;
    }
    try {
        const bool replay = argc == 3 && std::string(argv[2]) == "--replay";
        const bool benchmark = argc == 3 && std::string(argv[2]) == "--benchmark";
        json config;
        std::ifstream input(argv[1]);
        if (!input) throw std::runtime_error("cannot read pipeline config");
        input >> config;
        if (config.value("market", std::string()) != "SH") throw std::runtime_error("pipeline is Shanghai-only");
        JournalOptions journal_options;
        journal_options.directory = config.at("journal_directory").get<std::string>();
        journal_options.trading_day = config.at("trading_day").get<std::uint32_t>();
        journal_options.segment_bytes = config.value("journal_segment_bytes", journal_options.segment_bytes);
        journal_options.min_free_bytes = config.value("journal_min_free_bytes", journal_options.min_free_bytes);
        PipelineOptions options;
        options.shards = positive(config, "shard_count", options.shards);
        options.ingress_capacity = positive(config, "ingress_capacity", options.ingress_capacity);
        options.shard_capacity = positive(config, "shard_capacity", options.shard_capacity);
        options.journal_capacity = positive(config, "journal_capacity", options.journal_capacity);
        options.pending_snapshot_capacity = positive(config, "pending_snapshot_capacity", options.pending_snapshot_capacity);
        options.replay = replay;
        const bool model_enabled=config.count("model")!=0;
        options.tick_sampling=model_enabled;
        options.live_clock=!benchmark;
        if (options.shards > 128) throw std::runtime_error("too many SSE shards");
        const std::uint64_t wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (!replay && !benchmark && journal_options.trading_day != sse_live::local_trading_date(wall_ns))
            throw std::runtime_error("live capture requires today's trading_day");
        const std::string manifest_path = journal_options.directory + "/pipeline_manifest.json";
        if (replay) {
            json manifest;
            std::ifstream file(manifest_path.c_str());
            if (!file) throw std::runtime_error("missing SSE pipeline manifest");
            file >> manifest;
            if (manifest.at("shard_count").get<std::size_t>() != options.shards ||
                manifest.at("trading_day").get<std::uint32_t>() != journal_options.trading_day ||
                manifest.at("event_version").get<std::uint32_t>() != kEventVersion)
                throw std::runtime_error("replay must use the recorded day, event version and shard count");
            if (manifest.value("consumer",std::string("orderbook-validation")) !=
                (model_enabled ? "model" : "orderbook-validation") ||
                (model_enabled && manifest.at("model").dump()!=config.at("model").dump()))
                throw std::runtime_error("replay must use the recorded model configuration");
        }
        std::vector<deepwin_market_data::ChannelSpec> channels;
        std::map<std::string, std::size_t> channel_index;
        if (!replay && !benchmark) {
            for (const json& entry : config.at("channels")) {
                deepwin_market_data::ChannelSpec channel;
                channel.name = entry.at("name").get<std::string>();
                channel.group = entry.at("group").get<std::string>();
                channel.port = entry.at("port").get<int>();
                channel.interface_ip = entry.at("interface_ip").get<std::string>();
                channel.receive_cpu = entry.count("cpu") ? cpu_value(entry["cpu"]) : -1;
                if (channel.name.empty() || !channel_index.emplace(channel.name, channels.size()).second)
                    throw std::runtime_error("duplicate/empty receive channel name");
                channels.push_back(channel);
            }
            if (channels.empty()) throw std::runtime_error("no receive channels");
        }
        HistoryInput history;
        std::string history_error;
        double history_speed=1.0;
        std::uint64_t measure_start=0;
        if (benchmark) {
            const json& h=config.at("history");
            history_speed=h.value("speed",1.0);
            measure_start=h.value("measure_start_realtime_ns",h.at("start_realtime_ns").get<std::uint64_t>());
            options.latency_start_realtime_ns=measure_start;
            options.history_warmup_until_ns=measure_start;
            if (!(history_speed>0.0) || !std::isfinite(history_speed)) throw std::runtime_error("invalid history speed");
            if (!load_history(h.at("files").get<std::vector<std::string> >(),
                h.at("start_realtime_ns").get<std::uint64_t>(),h.at("stop_realtime_ns").get<std::uint64_t>(),
                journal_options.trading_day,&history,&history_error)) throw std::runtime_error(history_error);
        }
        options.producers = replay || benchmark ? 1 : channels.size();
        const std::size_t receive_count = channels.size();
        const std::size_t input_cpu_count=benchmark || replay ? 1 : receive_count;
        std::vector<int> requested(input_cpu_count + 2 + options.shards, -1);
        for (std::size_t i = 0; i < receive_count; ++i) requested[i] = channels[i].receive_cpu;
        if (config.count("cpus")) {
            const json& cpus = config["cpus"];
            if (cpus.count("input") && (benchmark || replay)) requested[0]=cpu_value(cpus["input"]);
            if (cpus.count("dispatcher")) requested[input_cpu_count] = cpu_value(cpus["dispatcher"]);
            if (cpus.count("journal")) requested[input_cpu_count + 1] = cpu_value(cpus["journal"]);
            if (cpus.count("shards")) {
                if (cpus["shards"].size() != options.shards) throw std::runtime_error("one CPU required per shard");
                for (std::size_t i = 0; i < options.shards; ++i) requested[input_cpu_count + 2 + i] = cpu_value(cpus["shards"][i]);
            }
        }
        std::string error;
        sse_cpu::Lease lease;
        if (!lease.acquire(requested, &error)) throw std::runtime_error(error);
        std::vector<int> worker_cpus;
        json affinity = json::array();
        for (std::size_t i = 0; i < requested.size(); ++i) {
            const sse_cpu::Cpu& cpu = lease.cpus()[i];
            const std::string role = i < input_cpu_count ? (benchmark || replay ? "input" : channels[i].name) :
                i == input_cpu_count ? "dispatcher" : i == input_cpu_count + 1 ? "journal" : "shard" + std::to_string(i - input_cpu_count - 2);
            affinity.push_back({{"role", role}, {"cpu", cpu.id}, {"l3", cpu.l3}});
            if (i < receive_count) channels[i].receive_cpu = cpu.id;
            else if(i>=input_cpu_count) worker_cpus.push_back(cpu.id);
        }
        EventJournal journal;
        EventJournalReader reader;
        if (replay ? !reader.open(journal_options, &error) : !journal.open(journal_options, &error))
            throw std::runtime_error(error);
        if (!replay) write_json(manifest_path, {{"trading_day", journal_options.trading_day},
            {"event_version", kEventVersion}, {"event_bytes", sizeof(Event)},
            {"shard_count", options.shards}, {"affinity", affinity}, {"consumer", model_enabled ? "model" : "orderbook-validation"},
            {"model", model_enabled ? config["model"] : json()}});
        std::vector<std::unique_ptr<BookConsumer> > books;
        std::vector<std::unique_ptr<ModelConsumer> > models;
        std::vector<EventConsumer> consumers;
        for (std::size_t i = 0; i < options.shards; ++i) {
            if (model_enabled) {
                models.emplace_back(new ModelConsumer(model_options(config["model"],journal_options.trading_day)));
                if (!models.back()->load(&error)) throw std::runtime_error(error);
                ModelConsumer* model=models.back().get();
                consumers.push_back([model](const Event& event) { return model->consume(event); });
            } else {
                books.emplace_back(new BookConsumer());
                BookConsumer* book = books.back().get();
                consumers.push_back([book](const Event& event) { return (*book)(event); });
            }
        }
        EventPipeline pipeline(options);
        std::chrono::steady_clock::time_point last_flush = std::chrono::steady_clock::now();
        EventConsumer persist = [&](const Event& event) {
            if (replay) return true;
            if (!journal.append(event, &error)) return false;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= std::chrono::milliseconds(100)) {
                last_flush = now;
                return journal.flush(&error);
            }
            return true;
        };
        if (!pipeline.start(consumers, persist, worker_cpus, &error)) throw std::runtime_error(error);
        std::atomic<std::uint64_t> malformed(0), filtered(0);
        bool input_ok = true;
        std::string input_error;
        LatencyHistogram injection_lag;
        std::map<std::uint64_t,std::uint64_t> second_counts;
        const auto run_started=std::chrono::steady_clock::now();
        double warmup_seconds=0;
        if ((benchmark || replay) && !sse_cpu::bind_current_thread(lease.cpus()[0].id,&input_error)) input_ok=false;
        if (benchmark && input_ok) {
            const std::uint64_t start=std::chrono::duration_cast<std::chrono::nanoseconds>(run_started.time_since_epoch()).count();
            std::uint64_t measure_epoch=0;
            for (const Event& event : history.events) {
                if(event.receive_realtime_ns<measure_start) {
                    while(!pipeline.submit(0,event)) std::this_thread::yield();
                    continue;
                }
                if(!measure_epoch) {
                    pipeline.drain();
                    measure_epoch=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    warmup_seconds=(measure_epoch-start)/1e9;
                }
                const auto due=measure_epoch+static_cast<std::uint64_t>((event.receive_realtime_ns-measure_start)/history_speed);
                std::uint64_t now;
                do {
                    now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (due>now+1000000) std::this_thread::sleep_for(std::chrono::nanoseconds(due-now-100000));
                } while(now<due);
                if(event.receive_realtime_ns>=measure_start) {
                    injection_lag.observe(now-due);
                    ++second_counts[event.receive_realtime_ns/1000000000ULL];
                }
                pipeline.submit(0,event,due);
            }
        }
        if (replay) {
            Event event;
            while (true) {
                const EventJournalReader::NextResult result = reader.next(&event, &input_error);
                if (result == EventJournalReader::End) break;
                if (result == EventJournalReader::Error) { input_ok = false; break; }
                // Replay must not lose data when computation is slower than
                // disk. Pace this producer without blocking live capture.
                while (!pipeline.submit(0, event)) std::this_thread::yield();
            }
        } else if (!benchmark) {
            if (!sse_cpu::bind_current_thread(lease.cpus()[0].id, &input_error)) input_ok = false;
            deepwin_market_data::UdpChannelRuntime runtime;
            std::signal(SIGINT, stop_receive);
            std::signal(SIGTERM, stop_receive);
            std::atomic<bool> receive_done(false);
            std::string control_error;
            std::thread control([&]() {
                sse_cpu::bind_current_thread(worker_cpus[1], &control_error);
                while (!receive_done.load()) {
                    if (stop_requested) runtime.stop();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
            try {
            if (input_ok) input_ok = runtime.run(channels, [&](const deepwin_market_data::Datagram& datagram) {
                const std::size_t producer = channel_index.at(datagram.channel);
                std::size_t offset = 0;
                while (offset < datagram.size) {
                    const std::size_t left = datagram.size - offset;
                    const unsigned char* record = datagram.data + offset;
                    if (left < 9) { ++malformed; break; }
                    const std::size_t size = record[8] == 0x3e ? 72 : record[8] == 0x27 ? 440 : 0;
                    if (size == 0 || left < size) { ++malformed; break; }
                    Event event;
                    std::string decode_error;
                    if (decode_event(record, size, datagram.receive_ns, datagram.monotonic_ns,
                                     journal_options.trading_day, &event, &decode_error)) pipeline.submit(producer, event);
                    else if (decode_error.find("security/time") != std::string::npos) ++filtered;
                    else ++malformed;
                    offset += size;
                }
            }, config.value("duration_ms", 0L), &input_error);
            } catch (...) {
                receive_done = true;
                control.join();
                throw;
            }
            receive_done = true;
            control.join();
            if (!control_error.empty()) { input_ok = false; input_error = control_error; }
        }
        pipeline.stop();
        const PipelineStats stats = pipeline.stats();
        const std::string append_error = error;
        bool close_ok = replay || journal.close(stats.journal_healthy && input_ok && malformed == 0, &error);
        json report = summarize(stats);
        report["mode"] = replay ? "replay" : benchmark ? "benchmark" : "live";
        report["elapsed_seconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-run_started).count();
        if (benchmark) {
            report["history_records"]=history.events.size(); report["history_rejected"]=history.rejected;
            report["history_speed"]=history_speed; report["injection_lag"]=latency_json(injection_lag);
            report["warmup_seconds"]=warmup_seconds; report["warmup_mode"]="lossless-before-measurement";
            report["measure_start_realtime_ns"]=measure_start;
            std::uint64_t max_rate=0,measured=0;
            for(const auto& entry : second_counts) { measured+=entry.second; max_rate=std::max(max_rate,entry.second); }
            report["measured_input_records"]=measured;report["peak_source_records_per_second"]=max_rate;
        }
        report["affinity"] = affinity;
        report["malformed"] = malformed.load(); report["filtered"] = filtered.load();
        report["input_ok"] = input_ok; report["input_error"] = input_error;
        report["pipeline_error"] = pipeline.error(); report["journal_error"] = error.empty() ? append_error : error;
        json stock_report = json::array();
        for (std::size_t shard = 0; shard < books.size(); ++shard)
            for (const auto& entry : books[shard]->stocks) {
                const Stock& stock = *entry.second;
                stock_report.push_back({{"symbol", entry.first}, {"channel_no", stock.channel},
                    {"shard", shard}, {"ticks", stock.ticks}, {"snapshots", stock.snapshots},
                    {"rejected", stock.rejected}, {"live_orders", stock.book.live_order_count()},
                    {"last_tick_index", stock.book.last_tick_index()}});
            }
        report["stocks"] = stock_report;
        json model_report=json::array();
        for (const auto& model : models) {
            const ModelStats& s=model->stats();
            model_report.push_back({{"ticks",s.ticks},{"snapshots",s.snapshots},{"samples",s.samples},
                {"predictions",s.predictions},{"snapshot_predictions",s.snapshot_predictions},{"selected",s.selected},
                {"tick_rejected",s.tick_rejected},{"model_errors",s.model_errors},{"missing_metadata",s.missing_metadata},
                {"missing_auction",s.missing_auction},{"prediction_sum",s.prediction_sum},{"quality",s.quality},
                {"observe",latency_json(s.observe)},{"factor",latency_json(s.factor)},{"infer",latency_json(s.infer)},
                {"snapshot_infer",latency_json(s.snapshot_infer)},{"stock_predictions",model->stock_predictions()}});
        }
        report["model"]=model_report;
        const std::string report_path=journal_options.directory+(replay ? "/replay_metrics.json" : "/metrics.json");
        write_json(report_path,report);
        std::cout << "events="<<stats.events<<" journal="<<stats.journal_written<<" compute_healthy="<<stats.compute_healthy
                  <<" journal_healthy="<<stats.journal_healthy<<" report="<<report_path<<'\n';
        return input_ok && close_ok && malformed == 0 && stats.compute_healthy && stats.journal_healthy ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "sse_event_pipeline: " << error.what() << '\n';
        return 2;
    }
}
