#include "../market_data/sse_event_codec.h"
#include "../market_data/sse_event_pipeline.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

const std::uint32_t kTradingDay = 20260907U;
const std::uint64_t kRealtimeBase = 1700000000000000000ULL;
const std::uint64_t kMonotonicBase = 9000000000000000000ULL;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put_u16(std::vector<unsigned char>* record, std::size_t offset,
             std::uint32_t value) {
    (*record)[offset] = static_cast<unsigned char>(value & 0xffU);
    (*record)[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void put_u32(std::vector<unsigned char>* record, std::size_t offset,
             std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index)
        (*record)[offset + index] = static_cast<unsigned char>(
            (value >> (index * 8U)) & 0xffU);
}

void put_u64(std::vector<unsigned char>* record, std::size_t offset,
             std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index)
        (*record)[offset + index] = static_cast<unsigned char>(
            (value >> (index * 8U)) & 0xffU);
}

std::string symbol(unsigned int value) {
    std::string result = std::to_string(value);
    check(result.size() <= 6U, "test symbol is too large");
    return std::string(6U - result.size(), '0') + result;
}

std::vector<unsigned char> raw_tick(std::uint32_t channel,
                                    const std::string& security,
                                    std::uint64_t sequence) {
    check(security.size() == 6U, "test tick symbol must be six digits");
    std::vector<unsigned char> record(72U, 0U);
    record[8U] = 0x3eU;
    put_u32(&record, 0U, static_cast<std::uint32_t>(sequence + 9U));
    put_u64(&record, 9U, sequence);
    put_u16(&record, 17U, channel);
    std::memcpy(&record[21U], security.data(), security.size());
    // 09:30:00.00 in the primary tick's HHMMSScc representation.
    put_u32(&record, 30U, 9300000U);
    record[34U] = 'T';
    for (std::size_t index = 35U; index < record.size(); ++index)
        record[index] = static_cast<unsigned char>((sequence + index * 3U) & 0xffU);
    return record;
}

std::vector<unsigned char> raw_snapshot(const std::string& security,
                                        std::uint32_t sequence) {
    check(security.size() == 6U, "test snapshot symbol must be six digits");
    std::vector<unsigned char> record(440U, 0U);
    record[8U] = 0x27U;
    put_u32(&record, 0U, sequence + 13U);
    put_u32(&record, 21U, sequence);
    put_u32(&record, 26U, 93000U);
    std::memcpy(&record[30U], security.data(), security.size());
    for (std::size_t index = 42U; index < record.size(); ++index)
        record[index] = static_cast<unsigned char>((sequence + index * 5U) & 0xffU);
    return record;
}

sse_pipeline::Event decode(const std::vector<unsigned char>& record,
                           std::uint64_t realtime_ns,
                           std::uint64_t monotonic_ns) {
    sse_pipeline::Event event;
    std::string error;
    check(sse_pipeline::decode_event(record.data(), record.size(), realtime_ns,
                                     monotonic_ns, kTradingDay, &event, &error),
          "raw SSE event did not decode");
    return event;
}

sse_pipeline::Event make_tick(std::uint32_t channel, const std::string& security,
                              std::uint64_t sequence, std::uint64_t clock_id = 0U) {
    return decode(raw_tick(channel, security, sequence),
                  kRealtimeBase + clock_id, kMonotonicBase + clock_id);
}

sse_pipeline::Event make_snapshot(const std::string& security,
                                  std::uint32_t sequence,
                                  std::uint64_t clock_id = 0U) {
    return decode(raw_snapshot(security, sequence),
                  kRealtimeBase + clock_id, kMonotonicBase + clock_id);
}

void check_payload_and_times(const sse_pipeline::Event& actual,
                             const sse_pipeline::Event& expected) {
    check(actual.receive_realtime_ns == expected.receive_realtime_ns,
          "receive realtime timestamp changed");
    check(actual.receive_mono_ns == expected.receive_mono_ns,
          "receive monotonic timestamp changed");
    check(actual.exchange_time_us == expected.exchange_time_us,
          "exchange timestamp changed");
    check(actual.sequence == expected.sequence, "sequence changed");
    check(actual.trading_day == expected.trading_day, "trading day changed");
    check(actual.payload_size == expected.payload_size, "payload size changed");
    check(actual.kind == expected.kind, "event kind changed");
    check(actual.flags == expected.flags, "event flags changed");
    check(actual.version == expected.version, "event version changed");
    check(std::memcmp(actual.security_id, expected.security_id,
                      sizeof(actual.security_id)) == 0,
          "security id changed");
    check(std::memcmp(actual.payload, expected.payload,
                      sizeof(actual.payload)) == 0,
          "event payload changed");
}

std::uint64_t sum(const std::vector<std::uint64_t>& values) {
    std::uint64_t result = 0U;
    for (std::size_t index = 0U; index < values.size(); ++index) result += values[index];
    return result;
}

void check_balanced_counts(const sse_pipeline::PipelineStats& stats,
                           std::size_t stocks_per_channel) {
    check(stats.channel_stock_counts.size() == 6U,
          "pipeline did not expose six channel count rows");
    for (std::size_t channel = 0U; channel < stats.channel_stock_counts.size(); ++channel) {
        const std::vector<std::size_t>& row = stats.channel_stock_counts[channel];
        check(row.size() == 4U, "pipeline count row has the wrong shard count");
        std::size_t minimum = row[0U];
        std::size_t maximum = row[0U];
        std::size_t total = 0U;
        for (std::size_t shard = 0U; shard < row.size(); ++shard) {
            if (row[shard] < minimum) minimum = row[shard];
            if (row[shard] > maximum) maximum = row[shard];
            total += row[shard];
        }
        check(total == stocks_per_channel && maximum - minimum <= 1U,
              "stocks were not balanced within a channel");
    }
}

bool wait_for_count(const std::atomic<unsigned int>& count,
                    unsigned int expected, unsigned int timeout_ms = 2000U) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (count.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return count.load(std::memory_order_acquire) >= expected;
}

class CallbackGate {
public:
    CallbackGate() : mutex_(), condition_(), entered_(false), released_(false) {}

    void block_once() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (entered_) return;
        entered_ = true;
        condition_.notify_all();
        condition_.wait_for(lock, std::chrono::milliseconds(2000U),
                            [this]() { return released_; });
    }

    bool wait_until_entered(unsigned int timeout_ms = 2000U) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                   [this]() { return entered_; });
    }

    void release() {
        std::unique_lock<std::mutex> lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_;
    bool released_;
};

void test_balancing_and_snapshot_routing() {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 4U;
    options.ingress_capacity = 256U;
    options.shard_capacity = 256U;
    options.journal_capacity = 256U;
    options.pending_snapshot_capacity = 256U;

    std::vector<std::vector<sse_pipeline::Event> > shard_events(options.shards);
    std::vector<sse_pipeline::Event> journal_events;
    std::vector<sse_pipeline::Event> expected;
    std::map<std::string, std::pair<std::uint32_t, std::uint32_t> > routes;
    std::map<std::string, unsigned int> tick_count;
    std::map<std::string, unsigned int> snapshot_count;
    std::vector<sse_pipeline::EventConsumer> consumers;
    for (std::size_t shard = 0U; shard < options.shards; ++shard) {
        consumers.push_back([&shard_events, shard](const sse_pipeline::Event& event) {
            shard_events[shard].push_back(event);
            return true;
        });
    }
    const sse_pipeline::EventConsumer journal =
        [&journal_events](const sse_pipeline::Event& event) {
            journal_events.push_back(event);
            return true;
        };

    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    try {
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "balancing pipeline start failed");
        std::uint64_t sequence = 1000U;
        unsigned int stock_index = 1U;
        for (std::uint32_t channel = 1U; channel <= 6U; ++channel) {
            for (unsigned int index = 0U; index < 5U; ++index) {
                const std::string name = symbol(600000U + stock_index++);
                ++sequence;
                const sse_pipeline::Event first = make_tick(
                    channel, name, sequence, sequence);
                ++sequence;
                const sse_pipeline::Event second = make_tick(
                    channel, name, sequence, sequence);
                const sse_pipeline::Event snapshot = make_snapshot(
                    name, static_cast<std::uint32_t>(900000U + sequence), sequence);
                expected.push_back(first);
                expected.push_back(second);
                expected.push_back(snapshot);
                check(pipeline.submit(0U, first), "first stock tick submit failed");
                check(pipeline.submit(0U, second), "second stock tick submit failed");
                check(pipeline.submit(0U, snapshot), "stock snapshot submit failed");
            }
        }
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(stats.submitted == expected.size(), "balancing submit count changed");
        check(stats.events == expected.size(), "balancing event count changed");
        check(stats.journal_written == expected.size(), "balancing journal count changed");
        check(sum(stats.shard_consumed) == expected.size(),
              "balancing shard delivery count changed");
        check_balanced_counts(stats, 5U);
        check(journal_events.size() == expected.size(),
              "balancing journal callback lost events");

        std::map<std::uint64_t, sse_pipeline::Event> expected_by_sequence;
        for (std::size_t index = 0U; index < expected.size(); ++index)
            expected_by_sequence[expected[index].sequence] = expected[index];
        for (std::size_t shard = 0U; shard < shard_events.size(); ++shard) {
            for (std::size_t index = 0U; index < shard_events[shard].size(); ++index) {
                const sse_pipeline::Event& event = shard_events[shard][index];
                check(event.shard_id == shard, "event reached the wrong shard callback");
                check(event.channel_no >= 1U && event.channel_no <= 6U,
                      "assigned event has an invalid channel");
                const std::map<std::uint64_t, sse_pipeline::Event>::const_iterator source =
                    expected_by_sequence.find(event.sequence);
                check(source != expected_by_sequence.end(),
                      "unexpected sequence reached shard callback");
                check_payload_and_times(event, source->second);
                const std::string name(event.security_id);
                const std::map<std::string, std::pair<std::uint32_t, std::uint32_t> >::iterator route =
                    routes.find(name);
                if (route == routes.end()) {
                    routes[name] = std::make_pair(static_cast<std::uint32_t>(event.channel_no),
                                                  event.shard_id);
                } else {
                    check(route->second.first == event.channel_no &&
                              route->second.second == event.shard_id,
                          "stock moved between ticks or snapshot");
                }
                if (event.kind == static_cast<std::uint8_t>(sse_pipeline::kTick))
                    ++tick_count[name];
                else if (event.kind == static_cast<std::uint8_t>(sse_pipeline::kSnapshot))
                    ++snapshot_count[name];
                else
                    check(false, "unexpected event kind in shard callback");
            }
        }
        check(routes.size() == 30U, "wrong number of routed stocks");
        for (std::map<std::string, std::pair<std::uint32_t, std::uint32_t> >::const_iterator it =
                 routes.begin(); it != routes.end(); ++it) {
            check(tick_count[it->first] == 2U, "stock did not retain both ticks");
            check(snapshot_count[it->first] == 1U, "stock snapshot was not routed");
        }
    } catch (...) {
        pipeline.stop();
        throw;
    }
}

void test_snapshot_before_tick_order_and_payload() {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 2U;
    options.ingress_capacity = 16U;
    options.shard_capacity = 16U;
    options.journal_capacity = 16U;
    options.pending_snapshot_capacity = 4U;

    const std::string name = "600901";
    const sse_pipeline::Event first_snapshot = make_snapshot(name, 501U, 1U);
    const sse_pipeline::Event second_snapshot = make_snapshot(name, 502U, 2U);
    const sse_pipeline::Event tick = make_tick(3U, name, 601U, 3U);
    std::vector<sse_pipeline::Event> shard_events;
    std::vector<sse_pipeline::Event> journal_events;
    const std::vector<sse_pipeline::Event> expected = {
        first_snapshot, second_snapshot, tick
    };
    std::vector<sse_pipeline::EventConsumer> consumers;
    consumers.push_back([&shard_events](const sse_pipeline::Event& event) {
        shard_events.push_back(event);
        return true;
    });
    consumers.push_back([](const sse_pipeline::Event&) { return true; });
    const sse_pipeline::EventConsumer journal =
        [&journal_events](const sse_pipeline::Event& event) {
            journal_events.push_back(event);
            return true;
        };

    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    try {
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "snapshot buffering pipeline start failed");
        check(pipeline.submit(0U, first_snapshot), "first pending snapshot submit failed");
        check(pipeline.submit(0U, second_snapshot), "second pending snapshot submit failed");
        check(pipeline.submit(0U, tick), "buffer release tick submit failed");
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(stats.events == 3U && stats.journal_written == 3U,
              "snapshot buffering event count changed");
        check(stats.pending_snapshots == 0U, "pending snapshots were not released");
        check(stats.routing_errors == 0U, "snapshot buffering raised a routing error");
        check(journal_events.size() == 3U, "snapshot journal order count changed");
        check(shard_events.size() == 3U, "snapshot shard order count changed");
        check(journal_events[0U].kind == sse_pipeline::kSnapshot &&
                  journal_events[1U].kind == sse_pipeline::kSnapshot &&
                  journal_events[2U].kind == sse_pipeline::kTick,
              "journal did not retain ingress order");
        check(journal_events[0U].channel_no == 0U &&
                  journal_events[1U].channel_no == 0U,
              "unmapped snapshots were journaled with a fake channel");
        check(journal_events[0U].shard_id == sse_pipeline::kUnassignedShard &&
                  journal_events[1U].shard_id == sse_pipeline::kUnassignedShard,
              "unmapped snapshots were journaled with a fake shard");
        check(shard_events[0U].sequence == 501U &&
                  shard_events[1U].sequence == 502U &&
                  shard_events[2U].sequence == 601U,
              "pending snapshots were not released in order");
        const std::uint32_t assigned_shard = shard_events[2U].shard_id;
        for (std::size_t index = 0U; index < shard_events.size(); ++index) {
            check(shard_events[index].channel_no == 3U &&
                      shard_events[index].shard_id == assigned_shard,
                  "buffered event route was not filled from its tick");
            check_payload_and_times(shard_events[index], expected[index]);
        }
    } catch (...) {
        pipeline.stop();
        throw;
    }
}

void test_channel_conflict_and_invalid_range() {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 2U;
    options.ingress_capacity = 16U;
    options.shard_capacity = 16U;
    options.journal_capacity = 16U;
    options.pending_snapshot_capacity = 4U;

    const std::string name = "600801";
    const sse_pipeline::Event first = make_tick(1U, name, 701U, 1U);
    const sse_pipeline::Event conflict = make_tick(2U, name, 702U, 2U);
    sse_pipeline::Event invalid_zero = make_tick(1U, "600802", 703U, 3U);
    sse_pipeline::Event invalid_high = make_tick(1U, "600803", 704U, 4U);
    invalid_zero.channel_no = 0U;
    invalid_high.channel_no = 7U;

    std::vector<sse_pipeline::EventConsumer> consumers;
    consumers.push_back([](const sse_pipeline::Event&) { return true; });
    consumers.push_back([](const sse_pipeline::Event&) { return true; });
    const sse_pipeline::EventConsumer journal =
        [](const sse_pipeline::Event&) { return true; };
    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    try {
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "validation pipeline start failed");
        check(pipeline.submit(0U, first), "conflict first submit failed");
        check(pipeline.submit(0U, conflict), "conflict second submit failed");
        check(pipeline.submit(0U, invalid_zero), "zero channel submit failed");
        check(pipeline.submit(0U, invalid_high), "high channel submit failed");
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(stats.submitted == 4U, "validation submit count changed");
        check(stats.events == 1U && stats.journal_written == 1U,
              "invalid/conflicting events reached the pipeline sinks");
        check(stats.routing_errors == 3U, "invalid/conflicting events were not rejected");
        check(sum(stats.shard_consumed) == 1U,
              "invalid/conflicting event reached a shard callback");
        check(!stats.compute_healthy && !stats.journal_healthy,
              "routing failure did not mark pipeline health");
        check(!pipeline.error().empty(), "routing failure did not retain an error");
        check(stats.channel_stock_counts.size() == 6U &&
                  stats.channel_stock_counts[0U][0U] +
                      stats.channel_stock_counts[0U][1U] == 1U,
              "conflicting channel changed stock counts");
        for (std::size_t channel = 1U; channel < stats.channel_stock_counts.size(); ++channel) {
            std::size_t total = 0U;
            for (std::size_t shard = 0U; shard < stats.channel_stock_counts[channel].size(); ++shard)
                total += stats.channel_stock_counts[channel][shard];
            check(total == 0U, "invalid channel created a stock count");
        }
    } catch (...) {
        pipeline.stop();
        throw;
    }
}

void test_replay_preserves_persisted_assignments() {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 3U;
    options.ingress_capacity = 16U;
    options.shard_capacity = 16U;
    options.journal_capacity = 16U;
    options.pending_snapshot_capacity = 4U;
    options.replay = true;

    sse_pipeline::Event first = make_tick(1U, "600701", 801U, 1U);
    sse_pipeline::Event second = make_tick(1U, "600702", 802U, 2U);
    sse_pipeline::Event third = make_tick(1U, "600703", 803U, 3U);
    sse_pipeline::Event snapshot = make_snapshot("600701", 901U, 4U);
    first.event_id = 1U;
    second.event_id = 2U;
    third.event_id = 3U;
    snapshot.event_id = 4U;
    first.shard_id = 2U;
    second.shard_id = 0U;
    third.shard_id = 1U;

    std::vector<std::vector<sse_pipeline::Event> > shard_events(options.shards);
    std::vector<sse_pipeline::Event> journal_events;
    std::vector<sse_pipeline::EventConsumer> consumers;
    for (std::size_t shard = 0U; shard < options.shards; ++shard) {
        consumers.push_back([&shard_events, shard](const sse_pipeline::Event& event) {
            shard_events[shard].push_back(event);
            return true;
        });
    }
    const sse_pipeline::EventConsumer journal =
        [&journal_events](const sse_pipeline::Event& event) {
            journal_events.push_back(event);
            return true;
        };

    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    try {
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "replay pipeline start failed");
        check(pipeline.submit(0U, first), "replay first submit failed");
        check(pipeline.submit(0U, second), "replay second submit failed");
        check(pipeline.submit(0U, third), "replay third submit failed");
        check(pipeline.submit(0U, snapshot), "replay snapshot submit failed");
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(stats.events == 4U && stats.routing_errors == 0U,
              "replay event sequence was rejected");
        check(stats.journal_written == 0U && sum(stats.shard_consumed) == 4U,
              "replay should bypass the journal while reaching all shards");
        check(stats.channel_stock_counts.size() == 6U &&
                  stats.channel_stock_counts[0U].size() == 3U &&
                  stats.channel_stock_counts[0U][0U] == 1U &&
                  stats.channel_stock_counts[0U][1U] == 1U &&
                  stats.channel_stock_counts[0U][2U] == 1U,
              "replay recomputed instead of restoring persisted shards");
        check(shard_events[0U].size() == 1U && shard_events[1U].size() == 1U &&
                  shard_events[2U].size() == 2U,
              "replay routes did not preserve persisted ownership");
        check(shard_events[0U][0U].sequence == 802U &&
                  shard_events[1U][0U].sequence == 803U &&
                  shard_events[2U][0U].sequence == 801U &&
                  shard_events[2U][1U].sequence == 901U,
              "replay event reached the wrong persisted shard");
        check(shard_events[2U][1U].channel_no == 1U,
              "snapshot lookup did not recover its persisted channel");
        check(journal_events.empty(),
              "replay unexpectedly invoked the journal callback");
    } catch (...) {
        pipeline.stop();
        throw;
    }
}

void test_replay_rejects_unassigned_tick() {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 2U;
    options.ingress_capacity = 4U;
    options.shard_capacity = 4U;
    options.journal_capacity = 4U;
    options.pending_snapshot_capacity = 2U;
    options.replay = true;

    const sse_pipeline::Event malformed = make_tick(1U, "600751", 811U, 1U);
    std::vector<std::vector<sse_pipeline::Event> > shard_events(options.shards);
    std::vector<sse_pipeline::Event> journal_events;
    std::vector<sse_pipeline::EventConsumer> consumers;
    for (std::size_t shard = 0U; shard < options.shards; ++shard) {
        consumers.push_back([&shard_events, shard](const sse_pipeline::Event& event) {
            shard_events[shard].push_back(event);
            return true;
        });
    }
    const sse_pipeline::EventConsumer journal =
        [&journal_events](const sse_pipeline::Event& event) {
            journal_events.push_back(event);
            return true;
        };

    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    try {
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "malformed replay pipeline start failed");
        check(pipeline.submit(0U, malformed), "malformed replay submit failed");
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(stats.submitted == 1U && stats.events == 0U &&
                  stats.routing_errors == 1U,
              "unassigned replay tick was not rejected as a routing error");
        check(stats.journal_written == 0U && journal_events.empty() &&
                  sum(stats.shard_consumed) == 0U,
              "malformed replay tick reached a sink");
        check(stats.channel_stock_counts.size() == 6U,
              "malformed replay changed channel count shape");
        for (std::size_t channel = 0U; channel < stats.channel_stock_counts.size(); ++channel) {
            std::size_t total = 0U;
            for (std::size_t shard = 0U; shard < stats.channel_stock_counts[channel].size(); ++shard)
                total += stats.channel_stock_counts[channel][shard];
            check(total == 0U, "malformed replay tick was assigned a new stock route");
        }
        check(!pipeline.error().empty(),
              "malformed replay rejection did not retain an error");
    } catch (...) {
        pipeline.stop();
        throw;
    }
}

void test_replay_preserves_late_tick_status() {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 2U;
    options.ingress_capacity = 4U;
    options.shard_capacity = 4U;
    options.journal_capacity = 4U;
    options.pending_snapshot_capacity = 2U;
    options.replay = true;

    sse_pipeline::Event expected = make_tick(2U, "600761", 821U, 1U);
    expected.event_id = 1U;
    expected.shard_id = 1U;
    expected.flags = static_cast<std::uint8_t>(sse_pipeline::kLateTick);

    std::vector<std::vector<sse_pipeline::Event> > shard_events(options.shards);
    std::vector<sse_pipeline::Event> journal_events;
    std::vector<sse_pipeline::EventConsumer> consumers;
    for (std::size_t shard = 0U; shard < options.shards; ++shard) {
        consumers.push_back([&shard_events, shard](const sse_pipeline::Event& event) {
            shard_events[shard].push_back(event);
            return true;
        });
    }
    const sse_pipeline::EventConsumer journal =
        [&journal_events](const sse_pipeline::Event& event) {
            journal_events.push_back(event);
            return true;
        };

    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    try {
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "late replay pipeline start failed");
        check(pipeline.submit(0U, expected), "late replay submit failed");
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(stats.events == 1U && stats.late_ticks == 1U &&
                  stats.routing_errors == 0U && !stats.compute_healthy,
              (std::string("replay late-tick status was not persisted: events=") + std::to_string(stats.events) +
              " late=" + std::to_string(stats.late_ticks) + " routing=" + std::to_string(stats.routing_errors) +
              " healthy=" + std::to_string(stats.compute_healthy) + " error=" + pipeline.error()).c_str());
        check(stats.journal_written == 0U && journal_events.empty(),
              "replay late tick was unexpectedly journaled");
        check(shard_events[0U].empty() && shard_events[1U].size() == 1U,
              "replay late tick changed its persisted shard route");
        const sse_pipeline::Event& actual = shard_events[1U][0U];
        check(actual.event_id == 1U && actual.kind == sse_pipeline::kTick &&
                  actual.payload_size == 72U && actual.channel_no == 2U &&
                  actual.shard_id == 1U &&
                  (actual.flags & static_cast<std::uint8_t>(sse_pipeline::kLateTick)) != 0U,
              "replay late tick envelope changed");
        check_payload_and_times(actual, expected);
        check(stats.channel_stock_counts.size() == 6U &&
                  stats.channel_stock_counts[1U][1U] == 1U,
              "replay late tick route was recomputed");
    } catch (...) {
        pipeline.stop();
        throw;
    }
}

void test_duplicate_tick_window_is_bounded() {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 1U;
    options.ingress_capacity = 16384U;
    options.shard_capacity = 16384U;
    options.journal_capacity = 16384U;
    options.pending_snapshot_capacity = 4U;

    std::atomic<unsigned int> consumed(0U);
    const sse_pipeline::EventConsumer consumer =
        [&consumed](const sse_pipeline::Event&) {
            consumed.fetch_add(1U, std::memory_order_release);
            return true;
        };
    const sse_pipeline::EventConsumer journal =
        [](const sse_pipeline::Event&) { return true; };
    std::vector<sse_pipeline::EventConsumer> consumers(1U, consumer);
    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    try {
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "dedup pipeline start failed");
        const std::string name = "600601";
        for (std::uint64_t sequence = 1U; sequence <= 8193U; ++sequence) {
            check(pipeline.submit(0U, make_tick(1U, name, sequence, sequence)),
                  "dedup sequence submit failed");
            if (sequence == 4U)
                check(pipeline.submit(0U, make_tick(1U, name, sequence, sequence + 10000U)),
                      "dedup duplicate submit failed");
        }
        // The 8193rd distinct sequence evicts sequence 1 from the bounded
        // window, so this old sequence is accepted as a late tick.
        check(pipeline.submit(0U, make_tick(1U, name, 1U, 20000U)),
              "bounded dedup re-entry submit failed");
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(stats.submitted == 8195U, "dedup submit count changed");
        check(stats.duplicates == 1U, "duplicate tick was not suppressed exactly once");
        check(stats.events == 8194U && stats.shard_consumed[0U] == 8194U,
              "bounded dedup event count changed");
        check(stats.late_ticks == 1U, "evicted duplicate was not recorded as late");
        check(consumed.load(std::memory_order_acquire) == 8194U,
              "dedup shard callback count changed");
    } catch (...) {
        pipeline.stop();
        throw;
    }
}

void test_journal_overflow_does_not_block_shards() {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 1U;
    options.ingress_capacity = 16U;
    options.shard_capacity = 8U;
    options.journal_capacity = 1U;
    options.pending_snapshot_capacity = 4U;

    CallbackGate gate;
    std::atomic<unsigned int> shard_count(0U);
    std::vector<sse_pipeline::EventConsumer> consumers;
    consumers.push_back([&shard_count](const sse_pipeline::Event&) {
        shard_count.fetch_add(1U, std::memory_order_release);
        return true;
    });
    std::vector<sse_pipeline::Event> journal_events;
    const sse_pipeline::EventConsumer journal =
        [&gate, &journal_events](const sse_pipeline::Event& event) {
            journal_events.push_back(event);
            gate.block_once();
            return true;
        };

    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    try {
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "journal overflow pipeline start failed");
        check(pipeline.submit(0U, make_tick(1U, "600501", 1001U, 1U)),
              "journal overflow first submit failed");
        check(gate.wait_until_entered(), "journal callback did not block");
        check(pipeline.submit(0U, make_tick(1U, "600502", 1002U, 2U)),
              "journal overflow second submit failed");
        check(pipeline.submit(0U, make_tick(1U, "600503", 1003U, 3U)),
              "journal overflow third submit failed");
        check(wait_for_count(shard_count, 1U),
              "shard callback did not progress while journal was blocked");
        gate.release();
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(stats.journal_overflow > 0U,
              "blocked journal did not report queue overflow");
        check(stats.shard_overflow[0U] == 0U && stats.shard_consumed[0U] == 3U,
              "journal overflow interrupted live shard delivery");
        check(journal_events.size() == 2U,
              "journal callback received an event after its bounded queue overflow");
    } catch (...) {
        gate.release();
        pipeline.stop();
        throw;
    }
}

void test_shard_overflow_does_not_block_journal() {
    sse_pipeline::PipelineOptions options;
    options.producers = 1U;
    options.shards = 1U;
    options.ingress_capacity = 16U;
    options.shard_capacity = 1U;
    options.journal_capacity = 8U;
    options.pending_snapshot_capacity = 4U;

    CallbackGate gate;
    std::atomic<unsigned int> journal_count(0U);
    std::vector<std::vector<sse_pipeline::Event> > shard_events(1U);
    std::vector<sse_pipeline::EventConsumer> consumers;
    consumers.push_back([&gate, &shard_events](const sse_pipeline::Event& event) {
        shard_events[0U].push_back(event);
        gate.block_once();
        return true;
    });
    const sse_pipeline::EventConsumer journal =
        [&journal_count](const sse_pipeline::Event&) {
            journal_count.fetch_add(1U, std::memory_order_release);
            return true;
        };

    sse_pipeline::EventPipeline pipeline(options);
    std::string error;
    try {
        check(pipeline.start(consumers, journal, std::vector<int>(), &error),
              "shard overflow pipeline start failed");
        check(pipeline.submit(0U, make_tick(1U, "600401", 1101U, 1U)),
              "shard overflow first submit failed");
        check(gate.wait_until_entered(), "shard callback did not block");
        check(pipeline.submit(0U, make_tick(1U, "600402", 1102U, 2U)),
              "shard overflow second submit failed");
        check(pipeline.submit(0U, make_tick(1U, "600403", 1103U, 3U)),
              "shard overflow third submit failed");
        check(wait_for_count(journal_count, 3U),
              "journal callback did not progress while shard was blocked");
        gate.release();
        pipeline.stop();

        const sse_pipeline::PipelineStats stats = pipeline.stats();
        check(stats.shard_overflow[0U] > 0U,
              "blocked shard did not report queue overflow");
        check(stats.journal_overflow == 0U && stats.journal_written == 3U,
              "shard overflow interrupted journal delivery");
        check(journal_count.load(std::memory_order_acquire) == 3U,
              "journal callback count changed after shard overflow");
        check(shard_events[0U].size() == 2U,
              "shard callback unexpectedly received an overflowed event");
    } catch (...) {
        gate.release();
        pipeline.stop();
        throw;
    }
}

}  // namespace

int main() {
    try {
        test_balancing_and_snapshot_routing();
        test_snapshot_before_tick_order_and_payload();
        test_channel_conflict_and_invalid_range();
        test_replay_preserves_persisted_assignments();
        test_replay_rejects_unassigned_tick();
        test_replay_preserves_late_tick_status();
        test_duplicate_tick_window_is_bounded();
        test_journal_overflow_does_not_block_shards();
        test_shard_overflow_does_not_block_journal();
        std::cout << "sse_event_pipeline_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
