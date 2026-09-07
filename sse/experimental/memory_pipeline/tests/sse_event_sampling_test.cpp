#include "../market_data/sse_event_pipeline.h"
#include "../market_data/sse_event_queue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

sse_pipeline::Event make_tick(const char* name, std::uint64_t sequence,
                              std::uint64_t receive_mono_ns,
                              std::uint16_t channel = 1U,
                              std::uint32_t shard = sse_pipeline::kUnassignedShard) {
    sse_pipeline::Event event = {};
    event.receive_realtime_ns = 1700000000000000000ULL + sequence;
    event.receive_mono_ns = receive_mono_ns;
    event.exchange_time_us = 34200000000ULL + sequence;
    event.sequence = sequence;
    event.trading_day = 20260907U;
    event.shard_id = shard;
    event.channel_no = channel;
    event.payload_size = 72U;
    event.kind = static_cast<std::uint8_t>(sse_pipeline::kTick);
    event.version = static_cast<std::uint16_t>(sse_pipeline::kEventVersion);
    std::memcpy(event.security_id, name, 6U);
    event.payload[8U] = 0x3eU;
    event.payload[17U] = static_cast<unsigned char>(channel & 0xffU);
    event.payload[18U] = static_cast<unsigned char>((channel >> 8U) & 0xffU);
    for (std::size_t index = 0U; index < event.payload_size; ++index)
        if (index != 8U && index != 17U && index != 18U)
            event.payload[index] = static_cast<unsigned char>((sequence + index) & 0xffU);
    return event;
}

struct RunResult {
    std::vector<sse_pipeline::Event> shard;
    std::vector<sse_pipeline::Event> journal;
    sse_pipeline::PipelineStats stats;
};

RunResult run_live(const std::vector<sse_pipeline::Event>& events,
                   std::size_t shard_capacity = 256U,
                   bool live_clock = false) {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 1U;
    options.ingress_capacity = 4096U;
    options.shard_capacity = shard_capacity;
    options.journal_capacity = 4096U;
    options.pending_snapshot_capacity = 64U;
    options.tick_sampling = true;
    options.live_clock = live_clock;

    RunResult result;
    std::vector<sse_pipeline::EventConsumer> consumers;
    consumers.push_back([&result](const sse_pipeline::Event& event) {
        result.shard.push_back(event);
        return true;
    });
    const sse_pipeline::EventConsumer journal =
        [&result](const sse_pipeline::Event& event) {
            result.journal.push_back(event);
            return true;
        };
    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    check(pipeline.start(consumers, journal, std::vector<int>(), &error),
          "sampling pipeline start failed");
    for (std::size_t index = 0U; index < events.size(); ++index) {
        const sse_pipeline::Event& event = events[index];
        check(pipeline.submit(0U, event, event.receive_mono_ns + 1U),
              "sampling event submit failed");
    }
    pipeline.stop();
    result.stats = pipeline.stats();
    return result;
}

void check_marker(const sse_pipeline::Event& marker,
                  const sse_pipeline::Event& source) {
    check(marker.kind == sse_pipeline::kTickSample, "event is not a tick sample");
    check(marker.payload_size == 72U, "sample payload size changed");
    check(marker.sequence == source.sequence, "sample sequence changed");
    check(marker.channel_no == source.channel_no && marker.shard_id == 0U,
          "sample route changed");
    check(marker.receive_realtime_ns == source.receive_realtime_ns,
          "sample realtime timestamp changed");
    check(marker.receive_mono_ns == source.receive_mono_ns + 101000U,
          "sample monotonic timestamp changed");
    check(std::memcmp(marker.security_id, source.security_id,
                      sizeof(marker.security_id)) == 0,
          "sample security changed");
    check(std::memcmp(marker.payload, source.payload, source.payload_size) == 0,
          "sample payload changed");
}

void test_queue_origin_roundtrip() {
    sse_pipeline::EventQueue queue(2U);
    const sse_pipeline::Event source = make_tick("600001", 1U, 1000U);
    sse_pipeline::Event actual = {};
    std::uint64_t origin = 0U;
    check(queue.push(source, 123456U), "origin queue push failed");
    check(queue.pop(&actual, &origin), "origin queue pop failed");
    check(origin == 123456U && actual.sequence == source.sequence,
          "origin metadata was not preserved");
    check(queue.push(source), "default origin queue push failed");
    origin = 99U;
    check(queue.pop(&actual, &origin) && origin == 0U,
          "default origin metadata was not zero");
}

void test_strict_quiet_boundary_and_marker_order() {
    const std::uint64_t base = 1000000000ULL;
    const std::vector<sse_pipeline::Event> events = {
        make_tick("600001", 1U, base),
        make_tick("600001", 2U, base + 100000U),
        make_tick("600001", 3U, base + 201000U)
    };
    const RunResult result = run_live(events);
    check(result.shard.size() == 5U, "unexpected strict-boundary event count");
    check(result.shard[0U].kind == sse_pipeline::kTick &&
              result.shard[0U].sequence == 1U,
          "first tick was not delivered first");
    check(result.shard[1U].kind == sse_pipeline::kTick &&
              result.shard[1U].sequence == 2U,
          "exact-100us tick was not delivered second");
    check(result.shard[2U].kind == sse_pipeline::kTickSample &&
              result.shard[2U].sequence == 2U,
          "101us marker was not emitted before the next tick");
    check(result.shard[3U].kind == sse_pipeline::kTick &&
              result.shard[3U].sequence == 3U,
          "next tick did not follow its marker");
    check_marker(result.shard[2U], events[1U]);
    check_marker(result.shard[4U], events[2U]);
    check(result.shard[2U].event_id == 3U && result.shard[3U].event_id == 4U &&
              result.shard[4U].event_id == 5U,
          "sample event IDs are not contiguous");
    check(result.journal.size() == result.shard.size(),
          "journal/live fanout counts differ");
    check(result.stats.events == 5U && result.stats.journal_written == 5U,
          "strict-boundary pipeline stats changed");
}

void test_noisy_stock_closes_quiet_timer() {
    const std::uint64_t base = 2000000000ULL;
    const std::vector<sse_pipeline::Event> events = {
        make_tick("600001", 1U, base),
        make_tick("600002", 2U, base + 101000U)
    };
    const RunResult result = run_live(events);
    check(result.shard.size() == 4U, "noisy-stock sample count changed");
    check(result.shard[0U].kind == sse_pipeline::kTick &&
              result.shard[0U].security_id[5] == '1',
          "first noisy-stock tick missing");
    check(result.shard[1U].kind == sse_pipeline::kTickSample &&
              result.shard[1U].security_id[5] == '1',
          "quiet stock marker was not emitted before noise");
    check(result.shard[2U].kind == sse_pipeline::kTick &&
              result.shard[2U].security_id[5] == '2',
          "noisy stock tick order changed");
    check(result.shard[3U].kind == sse_pipeline::kTickSample &&
              result.shard[3U].security_id[5] == '2',
          "final noisy-stock marker missing");
}

void test_timer_state_replaces_latest_tick() {
    const std::uint64_t base = 3000000000ULL;
    std::vector<sse_pipeline::Event> events;
    for (std::uint64_t sequence = 1U; sequence <= 2000U; ++sequence)
        events.push_back(make_tick("600003", sequence,
                                   base + sequence * 1000U));
    const RunResult result = run_live(events, 4096U);
    check(result.shard.size() == events.size() + 1U,
          "timer state accumulated stale markers");
    check(result.shard.back().kind == sse_pipeline::kTickSample &&
              result.shard.back().sequence == 2000U,
          "timer did not retain the latest tick");
    check(result.stats.events == events.size() + 1U,
          "timer replacement event count changed");
}

void test_replay_preserves_marker_without_regeneration() {
    const std::uint64_t base = 4000000000ULL;
    sse_pipeline::Event tick = make_tick("600004", 7U, base, 2U, 1U);
    sse_pipeline::Event marker = tick;
    marker.kind = static_cast<std::uint8_t>(sse_pipeline::kTickSample);
    marker.receive_mono_ns = base + 101000U;
    tick.event_id = 1U;
    marker.event_id = 2U;

    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 2U;
    options.ingress_capacity = 8U;
    options.shard_capacity = 1U;
    options.journal_capacity = 8U;
    options.pending_snapshot_capacity = 4U;
    options.replay = true;
    options.tick_sampling = true;
    options.live_clock = true;

    std::vector<sse_pipeline::Event> shard_zero;
    std::vector<sse_pipeline::Event> shard_one;
    std::vector<sse_pipeline::EventConsumer> consumers;
    consumers.push_back([&shard_zero](const sse_pipeline::Event& event) {
        shard_zero.push_back(event);
        return true;
    });
    consumers.push_back([&shard_one](const sse_pipeline::Event& event) {
        shard_one.push_back(event);
        return true;
    });
    const sse_pipeline::EventConsumer journal =
        [](const sse_pipeline::Event&) { return true; };
    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    check(pipeline.start(consumers, journal, std::vector<int>(), &error),
          "replay sampling pipeline start failed");
    check(pipeline.submit(0U, tick, 1U), "replay tick submit failed");
    check(pipeline.submit(0U, marker, 1U), "replay marker submit failed");
    pipeline.stop();
    const sse_pipeline::PipelineStats stats = pipeline.stats();
    check(shard_zero.empty() && shard_one.size() == 2U,
          "replay marker route was not preserved");
    check(shard_one[0U].kind == sse_pipeline::kTick &&
              shard_one[1U].kind == sse_pipeline::kTickSample,
          "replay marker was regenerated or reordered");
    check(stats.events == 2U && stats.journal_written == 0U &&
              stats.routing_errors == 0U,
          "replay marker stats changed");
    check(stats.shard_sample_end_to_end_latency.size() == 2U &&
              stats.shard_sample_end_to_end_latency[1U].count() == 1U,
          "replay sample latency was not measured");
}

void test_origin_latency_stats() {
    const std::vector<sse_pipeline::Event> events = {
        make_tick("600005", 1U, 5000000000ULL)
    };
    const RunResult result = run_live(events);
    check(result.stats.ingress_latency.count() == 1U,
          "dispatcher ingress latency was not recorded");
    check(result.stats.shard_service_latency.size() == 1U &&
              result.stats.shard_service_latency[0U].count() == 2U,
          "shard service latency was not recorded");
    check(result.stats.shard_end_to_end_latency.size() == 1U &&
              result.stats.shard_end_to_end_latency[0U].count() == 2U,
          "shard end-to-end latency did not include the marker");
    check(result.stats.shard_sample_end_to_end_latency.size() == 1U &&
              result.stats.shard_sample_end_to_end_latency[0U].count() == 1U,
              "sample end-to-end latency was not recorded");
}

void test_historical_warmup_backpressure_and_drain() {
    const std::uint64_t base_mono = 6000000000ULL;
    const std::uint64_t base_realtime = 1700000000000000000ULL;
    const std::uint64_t warmup_until = base_realtime + 100U;
    const std::vector<sse_pipeline::Event> warmup = {
        make_tick("600101", 1U, base_mono, 1U),
        make_tick("600101", 2U, base_mono + 100000U, 1U),
        make_tick("600102", 3U, base_mono + 201000U, 1U)
    };

    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 1U;
    options.ingress_capacity = 1U;
    options.shard_capacity = 1U;
    options.journal_capacity = 1U;
    options.pending_snapshot_capacity = 4U;
    options.tick_sampling = true;
    options.live_clock = false;
    options.history_warmup_until_ns = warmup_until;

    std::atomic<unsigned int> shard_seen(0U);
    std::atomic<unsigned int> journal_seen(0U);
    std::atomic<unsigned int> shard_samples(0U);
    std::atomic<unsigned int> journal_samples(0U);
    std::vector<sse_pipeline::EventConsumer> consumers;
    consumers.push_back([&shard_seen, &shard_samples](const sse_pipeline::Event& event) {
        shard_seen.fetch_add(1U, std::memory_order_release);
        if (event.kind == sse_pipeline::kTickSample)
            shard_samples.fetch_add(1U, std::memory_order_release);
        return true;
    });
    const sse_pipeline::EventConsumer journal =
        [&journal_seen, &journal_samples](const sse_pipeline::Event& event) {
            journal_seen.fetch_add(1U, std::memory_order_release);
            if (event.kind == sse_pipeline::kTickSample)
                journal_samples.fetch_add(1U, std::memory_order_release);
            return true;
        };

    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    check(pipeline.start(consumers, journal, std::vector<int>(), &error),
          "historical warmup pipeline start failed");
    const std::chrono::steady_clock::time_point submit_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (std::size_t index = 0U; index < 2U; ++index) {
        bool submitted = false;
        while (std::chrono::steady_clock::now() < submit_deadline) {
            if (pipeline.submit(0U, warmup[index], warmup[index].receive_mono_ns + 1U)) {
                submitted = true;
                break;
            }
            std::this_thread::yield();
        }
        check(submitted, "historical warmup submission timed out");
    }

    // The barrier follows both capacity-1 queues' accepted events. It must
    // not be visible to either callback or trigger the exact-100us timer.
    pipeline.drain();
    check(shard_seen.load(std::memory_order_acquire) == 2U &&
              journal_seen.load(std::memory_order_acquire) == 2U,
          "drain returned before historical callbacks progressed");
    check(shard_samples.load(std::memory_order_acquire) == 0U &&
              journal_samples.load(std::memory_order_acquire) == 0U,
          "drain barrier generated or exposed a premature sample");

    const std::chrono::steady_clock::time_point post_submit_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool submitted = false;
    while (std::chrono::steady_clock::now() < post_submit_deadline) {
        if (pipeline.submit(0U, warmup[2], warmup[2].receive_mono_ns + 1U)) {
            submitted = true;
            break;
        }
        std::this_thread::yield();
    }
    check(submitted, "post-drain historical submission timed out");
    pipeline.stop();

    const sse_pipeline::PipelineStats stats = pipeline.stats();
    check(stats.submitted == 3U && stats.events == 5U &&
              stats.journal_written == 5U,
          "historical drain event counts changed");
    check(stats.ingress_overflow == 0U && stats.journal_overflow == 0U &&
              stats.shard_overflow[0U] == 0U,
          "historical warmup reported queue overflow");
    check(shard_seen.load(std::memory_order_acquire) == 5U &&
              journal_seen.load(std::memory_order_acquire) == 5U &&
              shard_samples.load(std::memory_order_acquire) == 2U &&
              journal_samples.load(std::memory_order_acquire) == 2U,
          "historical warmup lost or duplicated post-drain samples");
    check(stats.compute_healthy && stats.journal_healthy,
          "historical warmup drain marked the pipeline unhealthy");
}

void test_immediate_stop_does_not_drop_final_event() {
    for (unsigned int iteration = 0U; iteration < 100U; ++iteration) {
        sse_pipeline::PipelineOptions options;
        options.producers = 1U;
        options.shards = 1U;
        options.ingress_capacity = 1U;
        options.shard_capacity = 1U;
        options.journal_capacity = 1U;
        options.pending_snapshot_capacity = 1U;
        options.tick_sampling = false;

        std::atomic<unsigned int> shard_seen(0U);
        std::atomic<unsigned int> journal_seen(0U);
        std::vector<sse_pipeline::EventConsumer> consumers;
        consumers.push_back([&shard_seen](const sse_pipeline::Event&) {
            shard_seen.fetch_add(1U, std::memory_order_release);
            return true;
        });
        const sse_pipeline::EventConsumer journal =
            [&journal_seen](const sse_pipeline::Event&) {
                journal_seen.fetch_add(1U, std::memory_order_release);
                return true;
            };

        sse_pipeline::EventPipeline pipeline(options);
        std::string error;
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "immediate-stop pipeline start failed");
        const sse_pipeline::Event event = make_tick(
            "600901", static_cast<std::uint64_t>(iteration + 1U),
            7000000000ULL + iteration);
        check(pipeline.submit(0U, event), "immediate-stop submit failed");
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(shard_seen.load(std::memory_order_acquire) == 1U &&
                  journal_seen.load(std::memory_order_acquire) == 1U,
              "immediate stop dropped or duplicated the final event");
        check(stats.submitted == 1U && stats.events == 1U &&
                  stats.journal_written == 1U &&
                  stats.shard_consumed.size() == 1U &&
                  stats.shard_consumed[0U] == 1U,
              "immediate-stop event stats changed");
        check(stats.ingress_overflow == 0U && stats.journal_overflow == 0U &&
                  stats.shard_overflow.size() == 1U &&
                  stats.shard_overflow[0U] == 0U &&
                  stats.compute_healthy && stats.journal_healthy,
              "immediate-stop pipeline reported an error");
    }
}

}  // namespace

int main() {
    try {
        test_queue_origin_roundtrip();
        test_strict_quiet_boundary_and_marker_order();
        test_noisy_stock_closes_quiet_timer();
        test_timer_state_replaces_latest_tick();
        test_replay_preserves_marker_without_regeneration();
        test_origin_latency_stats();
        test_historical_warmup_backpressure_and_drain();
        test_immediate_stop_does_not_drop_final_event();
        std::cout << "sse_event_sampling_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
