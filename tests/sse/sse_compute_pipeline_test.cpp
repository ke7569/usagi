#include "sse/runtime/sse_stream_processor.h"
#include "sse/runtime/sse_cpu_affinity.h"
#include "tests/sse/sse_test_artifacts.h"
#include "third_party/nlohmann/json.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace {
typedef std::vector<unsigned char> Bytes;
typedef deepwin_market_data::StreamEvent Event;
typedef sse_stream::SseStreamProcessor Processor;
typedef sse_stream::Output Output;

void put(Bytes& bytes, std::size_t offset, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) bytes[offset + i] = (value >> (i * 8)) & 255;
}
void append(Bytes& bytes, const Bytes& record) { bytes.insert(bytes.end(), record.begin(), record.end()); }
std::uint32_t wire_time(unsigned seconds) { return 9300000 + seconds / 60 * 10000 + seconds % 60 * 100; }
std::string symbol(unsigned index) { return std::to_string(600000 + index); }

Bytes tick(unsigned channel, std::uint64_t sequence, const std::string& code,
           char type = 'T', unsigned seconds = 1, unsigned side = 0) {
    Bytes bytes(72, 0);
    put(bytes, 0, sequence, 4); bytes[8] = 0x3e;
    put(bytes, 9, sequence, 8); put(bytes, 17, channel, 2);
    std::memcpy(bytes.data() + 21, code.data(), 6);
    put(bytes, 30, wire_time(seconds), 4); bytes[34] = type;
    put(bytes, 35, type == 'A' && side == 1 ? 0 : 1001, 8);
    put(bytes, 43, type == 'A' && side == 0 ? 0 : 2001, 8);
    put(bytes, 51, 10000, 4);
    put(bytes, 55, type == 'T' ? 100000 : 100000000, 8);
    bytes[71] = side;
    return bytes;
}
Bytes opening(unsigned channel = 1, const std::string& code = "600000",
              unsigned first_sequence = 1) {
    Bytes bytes = tick(channel, first_sequence, code, 'A', 0, 0);
    append(bytes, tick(channel, first_sequence + 1, code, 'A', 0, 1));
    return bytes;
}
Bytes heartbeat() {
    Bytes bytes(32, 0); bytes[8] = bytes[24] = 0xa2; return bytes;
}
Bytes snapshot(unsigned sequence, unsigned time, unsigned volume,
               const std::string& code = "600000") {
    Bytes bytes(440, 0); bytes[8] = 0x27;
    put(bytes, 0, sequence, 4); put(bytes, 21, sequence, 4);
    put(bytes, 26, time / 100, 4); std::memcpy(bytes.data() + 30, code.data(), 6);
    put(bytes, 42, 10000, 4); put(bytes, 46, 10000, 4);
    put(bytes, 50, 11000, 4); put(bytes, 54, 9000, 4); put(bytes, 58, 10500, 4);
    put(bytes, 74, static_cast<std::uint64_t>(volume) * 1000, 8);
    put(bytes, 82, static_cast<std::uint64_t>(volume) * 1000000, 8);
    for (unsigned i = 0; i < 5; ++i) {
        put(bytes, 124 + i * 16, 10000 - i * 10, 4);
        put(bytes, 128 + i * 16, 1000000, 8);
        put(bytes, 284 + i * 16, 11000 + i * 10, 4);
        put(bytes, 288 + i * 16, 1000000, 8);
    }
    return bytes;
}
Event datagram(const Bytes& bytes, std::uint64_t sequence, std::uint64_t mono,
               std::uint64_t hardware, std::uint64_t syscall = 1, unsigned index = 0) {
    Event event = {};
    event.kind = deepwin_market_data::kDatagramEvent;
    event.sequence = sequence; event.monotonic_ns = mono;
    event.realtime_ns = 100000000000ULL + mono;
    event.application_realtime_ns = event.realtime_ns;
    event.hardware_ns = hardware; event.hardware_clock_index = 3;
    event.timestamp_flags = deepwin_market_data::kKernelRealtimeTimestamp |
        deepwin_market_data::kHardwareReceiveTimestamp | deepwin_market_data::kHardwareTimestampRequested;
    event.receive_batch = syscall; event.batch_index = index; event.batch_size = 4;
    event.data = bytes.data(); event.size = bytes.size();
    return event;
}
Event idle(std::uint64_t sequence, std::uint64_t mono) {
    Event event = {}; event.kind = deepwin_market_data::kIdleEvent;
    event.sequence = sequence; event.monotonic_ns = mono; return event;
}
sse_tick::DailyStaticMetadataMap metadata(unsigned count = 1) {
    sse_tick::DailyStaticMetadata row;
    row.date = 20260909; row.avg_amount = 100000000; row.turnover_threshold = 0.1;
    row.free_share = 1000000; row.pre_close = 10; row.limit_price = 11; row.stop_price = 9;
    row.has_date = row.has_avg_amount = row.has_turnover_threshold = true;
    row.has_free_share = row.has_pre_close = row.has_limit_price = row.has_stop_price = true;
    row.quality = "pipeline-test";
    sse_tick::DailyStaticMetadataMap result;
    for (unsigned i = 0; i < count; ++i) result[symbol(i)] = row;
    return result;
}
sse_stream::PipelineConfig serial() {
    sse_stream::PipelineConfig config; config.contract = sse_stream::kHardwareBatchV3; return config;
}
sse_stream::PipelineConfig parallel(const std::vector<int>& cpus) {
    assert(cpus.size() == 4);
    sse_stream::PipelineConfig config = serial();
    config.book_cpus.assign(cpus.begin(), cpus.begin() + 2);
    config.inference_cpus.assign(cpus.begin() + 2, cpus.end());
    return config;
}

void check_latency_summary(const sse_stream::PipelineLatencyStats& latency,
                           bool require_samples) {
    if (require_samples) assert(latency.count > 0);
    assert(latency.p50_ns <= latency.p99_ns);
    assert(latency.count == 0 || latency.max_ns > 0);
    assert(latency.percentile_upper_bound);
}
sse_stream::Auction59Provider auction() {
    return [](const std::string&, std::uint64_t, std::vector<float>* output, std::string*) {
        output->assign(59, 0); return true;
    };
}
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}

struct Models {
    std::string directory;
    sse_hybrid_model::Model model;
    Models() {
        const char* real_profile = std::getenv("SSE_PIPELINE_REAL_PROFILE");
        if (real_profile) {
            std::ifstream input(real_profile);
            if (!input) throw std::runtime_error("cannot open SSE_PIPELINE_REAL_PROFILE");
            nlohmann::json profile; input >> profile;
            const auto& prediction = profile.at("prediction");
            std::string error;
            if (!model.load(prediction.at("model_path").get<std::string>(),
                    prediction.at("snapshot_baseline_model_path").get<std::string>(),
                    prediction.at("snapshot_baseline_scaler_path").get<std::string>(),
                    prediction.at("snapshot_auction59_model_path").get<std::string>(),
                    prediction.at("snapshot_auction59_scaler_path").get<std::string>(), &error))
                throw std::runtime_error(error);
            return;
        }
        char path[] = "/tmp/sse-pipeline-model-XXXXXX";
        char* created = mkdtemp(path); assert(created); directory = created;
        sse_test_artifacts::write_tick_artifact(directory + "/tick.bin");
        // Nonzero recurrent weights make missing/reordered steps observable
        // in subsequent predictions, unlike the constant-output base fixture.
        std::fstream weights((directory + "/tick.bin").c_str(), std::ios::in | std::ios::out | std::ios::binary);
        weights.seekg(0, std::ios::end);
        const std::size_t floats = (static_cast<std::size_t>(weights.tellg()) - 44) / 4;
        weights.seekp(44);
        for (std::size_t i = 0; i < floats; ++i) {
            const float value = static_cast<float>(static_cast<int>(i % 19) - 9) * 0.002f;
            weights.write(reinterpret_cast<const char*>(&value), 4);
        }
        weights.close();
        sse_test_artifacts::write_snapshot_artifact(directory + "/baseline.bin", 36);
        sse_test_artifacts::write_snapshot_artifact(directory + "/auction.bin", 95);
        sse_test_artifacts::write_scaler(directory + "/baseline.json", 36);
        sse_test_artifacts::write_scaler(directory + "/auction.json", 95);
        std::string error;
        assert(model.load(directory + "/tick.bin", directory + "/baseline.bin",
            directory + "/baseline.json", directory + "/auction.bin", directory + "/auction.json", &error));
    }
    ~Models() {
        if (directory.empty()) return;
        for (const char* file : {"tick.bin", "baseline.bin", "baseline.json", "auction.bin", "auction.json"})
            unlink((directory + "/" + file).c_str());
        rmdir(directory.c_str());
    }
};

void test_boundaries_and_software_contract() {
    std::vector<Output> outputs;
    Processor processor(metadata(), 0, true, [&](const Output& output) { outputs.push_back(output); }, auction(), serial());
    Bytes packet = opening();
    for (unsigned i = 3; i <= 20; ++i) append(packet, tick(1, i, "600000"));
    processor.on_event(datagram(packet, 1, 1000, 1000000, 7, 0));
    assert(outputs.empty());
    assert(processor.pipeline_stats().accepted_samples == 0);
    // A second hardware batch inside the same recvmmsg closes all twenty
    // records atomically, including the last trade in the first packet.
    Bytes trade = tick(1, 21, "600000", 'T', 2);
    processor.on_event(datagram(trade, 2, 2000, 1005000, 7, 1));
    assert(outputs.size() == 2 && outputs[0].tick.event.tick_index == 20);
    assert(outputs[1].batch_end.last_stream_sequence == 1);
    assert(outputs[1].batch_end.hardware_clock_index == 3);
    assert(outputs[1].batch_end.last_hardware_ns == 1000000);
    // The next syscall is still inside the current hardware batch.
    trade = tick(1, 22, "600000", 'T', 3);
    processor.on_event(datagram(trade, 3, 3000, 1005001, 8, 0));
    processor.on_event(idle(4, 7999));
    assert(outputs.size() == 2);
    processor.on_event(idle(5, 8000));
    assert(outputs.size() == 4 && outputs[2].tick.event.tick_index == 22);
    assert(outputs[3].batch_end.packet_count == 2);
    assert(outputs[3].batch_end.reason == sse_live_sampling::kBatchClosedByTimer);
    processor.finish(); processor.finish();

    outputs.clear();
    Processor software(metadata(), 0, true, [&](const Output& output) { outputs.push_back(output); });
    software.on_event(datagram(packet, 1, 1000, 9000000));
    software.on_event(idle(2, 100000));
    assert(outputs.empty());
    software.on_event(idle(3, 101001));
    assert(outputs.size() == 1 && outputs[0].kind == sse_stream::kTickOutput);
    assert(outputs[0].tick.provenance.processing_contract == sse_stream::kSoftwarePerInstrumentV2);
}

void test_full_feed_validation() {
    const Bytes first = tick(1, 1, "510050", 'S');
    for (int defect = 0; defect < 5; ++defect) {
        Processor processor(metadata(), 0, true, [](const Output&) {}, auction(), serial());
        processor.on_event(datagram(first, 1, 1000, 10000));
        const Bytes next = tick(1, 2, "510050", 'S');
        Event event = datagram(next, 2, 2000, 10001);
        if (defect == 0) event.hardware_ns = 0;
        if (defect == 1) event.timestamp_flags &= ~deepwin_market_data::kHardwareReceiveTimestamp;
        if (defect == 2) event.hardware_clock_index = 4;
        if (defect == 3) event.hardware_ns = 9999;
        if (defect == 4) event.monotonic_ns = 999;
        rejects([&]() { processor.on_event(event); });
        assert(processor.invalid());
        rejects([&]() { processor.finish(); });
    }
    Processor gap(metadata(), 0, true, [](const Output&) {}, auction(), serial());
    gap.on_event(datagram(first, 1, 1000, 10000));
    const Bytes missing = tick(1, 3, "510050", 'S');
    rejects([&]() { gap.on_event(datagram(missing, 2, 2000, 10001)); });

    std::vector<Output> outputs;
    Processor heartbeat_processor(metadata(), 0, true, [&](const Output& output) { outputs.push_back(output); }, auction(), serial());
    const Bytes pulse = heartbeat();
    heartbeat_processor.on_event(datagram(pulse, 1, 1000, 10000));
    heartbeat_processor.on_event(datagram(pulse, 2, 2000, 15000));
    assert(outputs.size() == 1 && outputs[0].batch_end.packet_count == 1);
    assert(outputs[0].batch_end.candidate_count == 0);
    Bytes corrupt(pulse); corrupt[24] = 0;
    rejects([&]() { heartbeat_processor.on_event(datagram(corrupt, 3, 3000, 16000)); });

    Processor changed(metadata(), 0, true, [](const Output&) {}, auction(), serial());
    const Bytes routed = opening();
    changed.on_event(datagram(routed, 1, 1000, 10000));
    const Bytes other_channel = tick(2, 1, "600000", 'S');
    rejects([&]() { changed.on_event(datagram(other_channel, 2, 2000, 10001)); });

    Processor mixed(metadata(), 0, true, [](const Output&) {}, auction(), serial());
    Bytes all_channels = tick(20, 1, "900001", 'S');
    append(all_channels, opening());
    append(all_channels, tick(20, 2, "900001", 'S'));
    mixed.on_event(datagram(all_channels, 1, 1000, 10000));
    mixed.on_event(idle(2, 6000));
    mixed.finish();
    assert(mixed.pipeline_stats().applied_ticks == 2);
}

void test_snapshot_marker_and_finish(const Models& models) {
    Bytes snapshots = snapshot(1, 9295900, 0);
    append(snapshots, snapshot(2, 9300000, 1));
    for (bool close : {false, true}) {
        std::vector<Output> outputs;
        Processor processor(metadata(), &models.model, false,
            [&](const Output& output) { outputs.push_back(output); }, auction(), serial());
        processor.on_event(datagram(snapshots, 1, 1000, 10000));
        assert(outputs.empty() && processor.pipeline_stats().inferred_samples == 0);
        if (close) processor.on_event(idle(2, 6000));
        processor.finish();
        assert(processor.pipeline_stats().retained_rows == 0);
        assert(outputs.size() == (close ? 2 : 0));
        if (close) {
            assert(outputs[0].kind == sse_stream::kSnapshotOutput);
            assert(outputs[0].snapshot.prediction_valid);
            assert(outputs[0].snapshot.provenance.batch_id == outputs[1].batch_end.batch_id);
            assert(processor.pipeline_stats().inferred_samples == 1);
        }
    }
}

void test_snapshot_first_route_and_frequency_weight() {
    sse_stream::PipelineConfig config = serial();
    config.inference_frequency_weights["600000"] = 3.0;
    std::vector<Output> outputs;
    Processor processor(metadata(), 0, true,
        [&](const Output& output) { outputs.push_back(output); }, auction(), config);

    Bytes snapshots = snapshot(1, 9295900, 0);
    append(snapshots, snapshot(2, 9300000, 1));
    processor.on_event(datagram(snapshots, 1, 1000, 10000));
    processor.on_event(idle(2, 6000));
    assert(!outputs.empty() && outputs[0].kind == sse_stream::kSnapshotOutput);

    // The first tick registers ChannelNo after snapshot routing has already
    // selected the stock's inference owner. That owner must remain stable.
    processor.on_event(datagram(opening(), 3, 7000, 16000));
    processor.on_event(datagram(heartbeat(), 4, 8000, 21000));
    processor.finish();
    const sse_stream::PipelineStats stats = processor.pipeline_stats();
    assert(stats.inference_channel_shard_counts.size() == 6);
    assert(stats.inference_channel_shard_counts[0].size() == 1);
    assert(stats.inference_channel_shard_counts[0][0] == 1);
    assert(stats.accepted_samples == 1 && stats.inference_samples.size() == 1 &&
           stats.inference_samples[0] == 1);
}

void test_independent_udp_subscriptions() {
    std::vector<Output> outputs;
    Processor processor(metadata(), 0, true, [&](const Output& output) { outputs.push_back(output); }, auction(), serial());
    Bytes ticks = opening(); append(ticks, tick(1, 3, "600000", 'T', 1));
    Event tick_event = datagram(ticks, 1, 1000, 100000);
    tick_event.channel_id = 0;
    processor.on_event(tick_event);
    Bytes snapshots = snapshot(1, 9295900, 0);
    append(snapshots, snapshot(2, wire_time(0), 1));
    Event snapshot_event = datagram(snapshots, 2, 2000, 50000);
    snapshot_event.channel_id = 1; // An older PHC stamp from another socket is valid.
    processor.on_event(snapshot_event);
    const Bytes pulse = heartbeat();
    Event boundary = datagram(pulse, 3, 3000, 55000);
    boundary.channel_id = 1;
    processor.on_event(boundary);
    assert(outputs.size() == 2 && outputs[0].kind == sse_stream::kSnapshotOutput);
    assert(outputs[1].batch_end.stream_channel_id == 1);
    assert(processor.pipeline_stats().accepted_samples == 1); // tick cut remains open.
    const Bytes next_tick = tick(1, 4, "600000", 'T', 2);
    tick_event = datagram(next_tick, 4, 4000, 100001);
    processor.on_event(tick_event);
    assert(outputs.size() == 2);
    boundary = datagram(pulse, 5, 5000, 105001);
    processor.on_event(boundary);
    assert(outputs.size() == 4 && outputs[2].tick.event.tick_index == 4);
    assert(outputs[3].batch_end.stream_channel_id == 0 && outputs[3].batch_end.packet_count == 2);
    processor.finish();
}

struct Recording {
    std::vector<Bytes> payloads;
    void send(Processor& processor) const {
        for (std::size_t i = 0; i < payloads.size(); ++i)
            processor.on_event(datagram(payloads[i], i + 1, 1000 + i * 1000,
                1000000 + i * 10000, i / 3, i % 3));
        processor.on_event(idle(payloads.size() + 1, 1000 + payloads.size() * 1000 + 5000));
    }
};
Recording recording() {
    Recording recording;
    Bytes previous, current, orders;
    std::uint64_t sequence[3] = {};
    for (unsigned stock = 0; stock < 4; ++stock) {
        const unsigned channel = stock / 2 + 1;
        append(previous, snapshot(1, 9295900, 0, symbol(stock)));
        append(current, snapshot(2, 9300000, 1, symbol(stock)));
        append(orders, opening(channel, symbol(stock), sequence[channel] + 1));
        sequence[channel] += 2;
    }
    recording.payloads.push_back(previous);
    recording.payloads.push_back(current);
    recording.payloads.push_back(orders);
    for (unsigned step = 1; step <= 60; ++step) {
        Bytes packet;
        for (unsigned stock = 0; stock < 4; ++stock) {
            const unsigned channel = stock / 2 + 1;
            append(packet, tick(channel, ++sequence[channel], symbol(stock), 'T', step));
            if (step % 10 == 0) append(packet, snapshot(2 + step / 10,
                wire_time(step), step * 100 + 1, symbol(stock)));
        }
        recording.payloads.push_back(packet);
    }
    return recording;
}

std::vector<Output> run(const Recording& recording, const Models& models,
                        const sse_stream::PipelineConfig& config, const char* label) {
    std::vector<Output> outputs;
    const std::thread::id owner = std::this_thread::get_id();
    Processor processor(metadata(4), &models.model, false, [&](const Output& output) {
        assert(std::this_thread::get_id() == owner); outputs.push_back(output);
    }, auction(), config);
    std::atomic<bool> inspecting(true);
    std::thread inspector([&]() {
        while (inspecting.load()) {
            const auto stats = processor.pipeline_stats();
            assert(stats.retained_rows <= config.output_capacity);
            assert(stats.retained_row_high_water <= config.output_capacity);
            assert(stats.channel_shard_counts.size() == 6);
            std::this_thread::yield();
        }
    });
    const auto start = std::chrono::steady_clock::now();
    recording.send(processor);
    const auto ingested = std::chrono::steady_clock::now();
    processor.finish();
    inspecting.store(false); inspector.join();
    const auto finished = std::chrono::steady_clock::now();
    const sse_stream::PipelineStats stats = processor.pipeline_stats();
    assert(stats.applied_ticks == 248);
    assert(stats.retained_rows == 0);
    std::size_t samples = 0;
    std::map<std::string, std::uint64_t> previous;
    for (const Output& output : outputs) {
        if (output.kind == sse_stream::kBatchEndOutput) continue;
        ++samples;
        if (output.kind == sse_stream::kTickOutput) {
            assert(output.tick.event.tick_index > previous[output.tick.event.security_id]);
            previous[output.tick.event.security_id] = output.tick.event.tick_index;
        }
    }
    assert(samples == 268 && stats.accepted_samples == samples && stats.inferred_samples == samples);
    assert(stats.inference_channel_shard_counts.size() == 6);
    assert(stats.inference_samples.size() ==
           (config.inference_cpus.empty() ? 1U : config.inference_cpus.size()));
    std::uint64_t worker_samples = 0;
    for (std::size_t i = 0; i < stats.inference_samples.size(); ++i)
        worker_samples += stats.inference_samples[i];
    assert(worker_samples == samples);
    assert(stats.inference_batch_completion.size() == stats.inference_samples.size());
    for (std::size_t i = 0; i < stats.inference_batch_completion.size(); ++i)
        check_latency_summary(stats.inference_batch_completion[i], stats.inference_samples[i] > 0);
    check_latency_summary(stats.batch_completion, stats.closed_batches > 0);
    if (!config.book_cpus.empty()) {
        assert(stats.book_cpus == config.book_cpus && stats.inference_cpus == config.inference_cpus);
        assert(stats.channel_shard_counts[0][0] == 1 && stats.channel_shard_counts[0][1] == 1);
        assert(stats.channel_shard_counts[1][0] == 1 && stats.channel_shard_counts[1][1] == 1);
        assert(stats.inference_channel_shard_counts[0].size() == config.inference_cpus.size());
        assert(stats.inference_channel_shard_counts[1].size() == config.inference_cpus.size());
        assert(stats.inference_channel_shard_counts[0][0] == 1 &&
               stats.inference_channel_shard_counts[0][1] == 1);
        assert(stats.inference_channel_shard_counts[1][0] == 1 &&
               stats.inference_channel_shard_counts[1][1] == 1);
        assert(stats.inference_queue_sizes.size() == config.inference_cpus.size());
        assert(stats.inference_queue_high_water.size() == config.inference_cpus.size());
        assert(stats.inference_queue_wait.size() == config.inference_cpus.size());
        for (std::size_t i = 0; i < stats.inference_queue_wait.size(); ++i)
            check_latency_summary(stats.inference_queue_wait[i], true);
    }
    std::cout << label << " input_us=" << std::chrono::duration_cast<std::chrono::microseconds>(ingested - start).count()
              << " drained_us=" << std::chrono::duration_cast<std::chrono::microseconds>(finished - start).count()
              << " ticks=" << stats.applied_ticks << " samples=" << samples << '\n';
    return outputs;
}

void compare(const std::vector<Output>& a, const std::vector<Output>& b) {
    assert(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        assert(a[i].kind == b[i].kind);
        if (a[i].kind == sse_stream::kTickOutput) {
            const auto& x = a[i].tick; const auto& y = b[i].tick;
            assert(x.event.security_id == y.event.security_id && x.event.tick_index == y.event.tick_index);
            assert(x.provenance.batch_id == y.provenance.batch_id);
            assert(x.provenance.stream_sequence == y.provenance.stream_sequence);
            assert(x.factors.values == y.factors.values);
            assert(x.prediction_valid == y.prediction_valid);
            assert(x.prediction.tick_generated == y.prediction.tick_generated &&
                   x.prediction.snapshot_generated == y.prediction.snapshot_generated &&
                   x.prediction.selected == y.prediction.selected &&
                   x.prediction.selected_source == y.prediction.selected_source &&
                   x.prediction.tick_pred == y.prediction.tick_pred &&
                   x.prediction.snapshot_pred == y.prediction.snapshot_pred &&
                   x.prediction.selected_pred == y.prediction.selected_pred);
        } else if (a[i].kind == sse_stream::kSnapshotOutput) {
            const auto& x = a[i].snapshot; const auto& y = b[i].snapshot;
            assert(x.snapshot.security_id == y.snapshot.security_id);
            assert(x.provenance.batch_id == y.provenance.batch_id &&
                   x.provenance.stream_sequence == y.provenance.stream_sequence);
            assert(x.snapshot36 == y.snapshot36 && x.prediction_valid == y.prediction_valid);
            assert(x.prediction.tick_generated == y.prediction.tick_generated &&
                   x.prediction.snapshot_generated == y.prediction.snapshot_generated &&
                   x.prediction.selected == y.prediction.selected &&
                   x.prediction.selected_source == y.prediction.selected_source &&
                   x.prediction.tick_pred == y.prediction.tick_pred &&
                   x.prediction.snapshot_pred == y.prediction.snapshot_pred &&
                   x.prediction.selected_pred == y.prediction.selected_pred);
        } else {
            assert(a[i].batch_end.batch_id == b[i].batch_end.batch_id);
            assert(a[i].batch_end.last_stream_sequence == b[i].batch_end.last_stream_sequence);
            assert(a[i].batch_end.prediction_count == b[i].batch_end.prediction_count);
        }
    }
}

void test_balanced_channels(const sse_stream::PipelineConfig& config) {
    Processor processor(metadata(24), 0, true, [](const Output&) {}, auction(), config);
    Bytes packet;
    for (unsigned channel = 1; channel <= 6; ++channel)
        for (unsigned stock = 0; stock < 4; ++stock)
            append(packet, opening(channel, symbol((channel - 1) * 4 + stock), stock * 2 + 1));
    processor.on_event(datagram(packet, 1, 1000, 10000));
    processor.on_event(idle(2, 6000)); processor.finish();
    const auto stats = processor.pipeline_stats();
    assert(stats.applied_ticks == 48);
    for (unsigned channel = 0; channel < 6; ++channel) {
        assert(stats.channel_shard_counts[channel].size() == 2);
        assert(stats.channel_shard_counts[channel][0] == 2 && stats.channel_shard_counts[channel][1] == 2);
    }
}

void test_busy_inference_keeps_books_moving(const Models& models,
                                           const sse_stream::PipelineConfig& config) {
    Processor processor(metadata(2), &models.model, false, [](const Output&) {}, auction(), config);
    Bytes orders = opening(); append(orders, opening(1, "600001", 3));
    processor.on_event(datagram(orders, 1, 1000, 10000));
    unsigned sequence = 1;
    for (unsigned packet = 0; packet < 8; ++packet) {
        Bytes snapshots;
        for (unsigned i = 0; i < 40; ++i) {
            const unsigned second = packet * 40 + i;
            append(snapshots, snapshot(sequence++, wire_time(second), second));
        }
        processor.on_event(datagram(snapshots, 2 + packet, 2000 + packet * 1000, 20000 + packet));
    }
    const Bytes pulse = heartbeat();
    processor.on_event(datagram(pulse, 10, 10000, 30000));
    // Three hundred nineteen queued snapshot recurrences occupy inference 0.
    // These subsequent status ticks must still reach both book shards.
    Bytes status;
    for (unsigned i = 0; i < 100; ++i)
        append(status, tick(1, 5 + i, i % 2 ? "600001" : "600000", 'S', 321));
    processor.on_event(datagram(status, 11, 11000, 30001));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool observed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto stats = processor.pipeline_stats();
        if (stats.applied_ticks == 104) {
            observed = stats.inferred_samples < 319;
            break;
        }
        processor.poll_outputs(); std::this_thread::yield();
    }
    assert(observed);
    processor.finish();
    assert(processor.pipeline_stats().inferred_samples == 319);
}

void test_failure_and_owner(const sse_stream::PipelineConfig& config) {
    {
        Processor processor(metadata(), 0, true, [](const Output&) { throw std::runtime_error("callback sentinel"); }, auction(), config);
        const Bytes pulse = heartbeat();
        processor.on_event(datagram(pulse, 1, 1000, 10000));
        rejects([&]() { processor.on_event(idle(2, 6000)); });
        rejects([&]() { processor.finish(); });
        assert(processor.invalid_reason() == "callback sentinel");
    }
    {
        Processor processor(metadata(), 0, true, [](const Output&) {}, auction(), config);
        processor.poll_outputs();
        std::thread wrong([&]() { rejects([&]() { processor.poll_outputs(); }); });
        wrong.join(); assert(processor.invalid());
        rejects([&]() { processor.finish(); });
    }
    {
        sse_stream::PipelineConfig bounded = config; bounded.output_capacity = 1;
        Processor processor(metadata(2), 0, true, [](const Output&) {}, auction(), bounded);
        Bytes orders = opening(); append(orders, opening(1, "600001", 3));
        processor.on_event(datagram(orders, 1, 1000, 10000));
        rejects([&]() { processor.on_event(idle(2, 6000)); });
        assert(processor.invalid_reason().find("capacity") != std::string::npos);
        rejects([&]() { processor.finish(); });
    }
    if (!config.book_cpus.empty()) {
        sse_stream::PipelineConfig duplicate = config;
        duplicate.inference_cpus[0] = duplicate.book_cpus[0];
        rejects([&]() { Processor processor(metadata(), 0, true, [](const Output&) {}, auction(), duplicate); });
        // Reacquisition after failures proves workers joined and L3 leases released.
        Processor processor(metadata(), 0, true, [](const Output&) {}, auction(), config);
        processor.finish();
    }
}

void test_retained_rows_remain_bounded(const Models& models,
                                     const sse_stream::PipelineConfig& base) {
    sse_stream::PipelineConfig config = base;
    config.output_capacity = 64;
    Processor processor(metadata(), &models.model, false, [](const Output&) {}, auction(), config);
    Bytes snapshots = snapshot(1, 9295900, 0);
    for (unsigned i = 0; i < 63; ++i) append(snapshots, snapshot(i + 2, wire_time(i), i + 1));
    processor.on_event(datagram(snapshots, 1, 1000, 10000));
    const Bytes pulse = heartbeat();
    processor.on_event(datagram(pulse, 2, 2000, 20000));
    assert(processor.pipeline_stats().retained_rows == 63);
    Bytes next = snapshot(65, wire_time(63), 64);
    append(next, snapshot(66, wire_time(64), 65));
    rejects([&]() { processor.on_event(datagram(next, 3, 3000, 20001)); });
    assert(processor.invalid_reason().find("retained sample capacity") != std::string::npos);
    rejects([&]() { processor.finish(); });
    assert(processor.pipeline_stats().retained_row_high_water == 64);
    assert(processor.pipeline_stats().retained_rows == 0);
}

}  // namespace

int main() {
    Models models;
    test_boundaries_and_software_contract();
    test_full_feed_validation();
    test_snapshot_marker_and_finish(models);
    test_snapshot_first_route_and_frequency_weight();
    test_independent_udp_subscriptions();
    test_failure_and_owner(serial());
    const Recording input = recording();
    const auto reference = run(input, models, serial(), "serial-v3");
    const char* requested = std::getenv("SSE_PIPELINE_TEST_CPUS");
    if (requested) {
        std::vector<int> cpus; std::string error;
        assert(sse_cpu::parse_cpu_list(requested, &cpus, &error) && cpus.size() == 4);
        const auto config = parallel(cpus);
        compare(reference, run(input, models, config, "parallel-v3"));
        test_balanced_channels(config);
        test_busy_inference_keeps_books_moving(models, config);
        test_failure_and_owner(config);
        test_retained_rows_remain_bounded(models, config);
    } else std::cout << "Set SSE_PIPELINE_TEST_CPUS to four distinct L3 CPUs for concurrency checks.\n";
    std::cout << "sse_compute_pipeline_test: PASS\n";
}
