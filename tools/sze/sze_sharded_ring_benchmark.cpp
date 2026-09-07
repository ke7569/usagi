#include "sze/market_data/SZEDirectShardedRing.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct Metrics {
    std::vector<std::uint64_t> publish_ns;
    std::vector<std::uint64_t> consume_ns;
    std::vector<std::uint64_t> total_ns;
    std::uint64_t elapsed_ns = 0U;
    std::uint64_t overruns = 0U;
};

std::uint64_t percentile(std::vector<std::uint64_t> values, double quantile)
{
    if (values.empty()) return 0U;
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        quantile * static_cast<double>(values.size() - 1U));
    return values[index];
}

void print_metric(const char* path, const char* stage,
                  const std::vector<std::uint64_t>& values)
{
    std::cout << " path=" << path << " stage=" << stage
              << " p50_ns=" << percentile(values, 0.50)
              << " p95_ns=" << percentile(values, 0.95)
              << " p99_ns=" << percentile(values, 0.99)
              << " p99_9_ns=" << percentile(values, 0.999)
              << " max_ns=" << percentile(values, 1.0) << '\n';
}

void make_event(std::uint64_t id, sze_recovery::CanonicalEvent* event,
                unsigned char* payload, std::size_t payload_bytes)
{
    std::memset(event, 0, sizeof(*event));
    std::memset(payload, static_cast<int>(id & 0xffU), payload_bytes);
    event->event_id = id;
    event->feed_sequence = id + 1000U;
    event->channel_sequence = id;
    event->receive_mono_ns = sze_recovery::monotonic_time_ns();
    event->trading_day = 20260814U;
    event->source_id = 88U;
    event->payload_size = static_cast<std::uint16_t>(payload_bytes);
    event->message_type = 23U;
    event->record_kind = sze_recovery::kRecordMarketData;
}

}  // namespace

int main(int argc, char** argv)
{
    std::uint64_t count = 300000U;
    std::uint64_t peak_events_per_second = 200000U;
    bool enforce = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--events" && i + 1 < argc) {
            count = std::strtoull(argv[++i], 0, 10);
        } else if (arg == "--peak-events-per-second" && i + 1 < argc) {
            peak_events_per_second = std::strtoull(argv[++i], 0, 10);
        } else if (arg == "--enforce") {
            enforce = true;
        } else {
            std::cerr << "usage: sze_sharded_ring_benchmark [--events N] "
                         "[--peak-events-per-second N] [--enforce]" << std::endl;
            return 2;
        }
    }
    if (count == 0U) return 2;

    char global_path[128];
    std::snprintf(global_path, sizeof(global_path),
                  "/dev/shm/sze_ring_ab_%ld_global", static_cast<long>(::getpid()));
    sze_recovery::RingConfig global_config;
    global_config.path = global_path;
    global_config.trading_day = 20260814U;
    global_config.source_id = 88U;
    global_config.capacity = 1024U;
    global_config.max_payload_bytes = 128U;
    global_config.generation = 9U;
    sze_recovery::ShmEventRing global_producer;
    sze_recovery::ShmEventRing global_consumer;
    if (!global_producer.create(global_config) ||
        !global_consumer.attach(global_path)) return 3;

    const std::uint32_t shard_count = 8U;
    sze_sharded_ring::DirectShardedRingConfig shard_config;
    shard_config.enabled = true;
    shard_config.trading_day = 20260814U;
    shard_config.source_id = 88U;
    shard_config.shard_count = shard_count;
    shard_config.capacity_per_shard = 1024U;
    shard_config.max_payload_bytes = 128U;
    shard_config.generation = 9U;
    for (std::uint32_t shard = 0; shard < shard_count; ++shard) {
        char path[128];
        std::snprintf(path, sizeof(path), "/dev/shm/sze_ring_ab_%ld_%u",
                      static_cast<long>(::getpid()), shard);
        shard_config.paths.push_back(path);
    }
    for (std::uint32_t symbol = 0; symbol < 3000U; ++symbol) {
        shard_config.assignments.push_back(
            std::make_pair(symbol, symbol % shard_count));
    }
    sze_sharded_ring::DirectShardedRingProducer shard_producer;
    if (!shard_producer.create(shard_config)) return 4;
    std::vector<sze_sharded_ring::ShardEventRing> shard_consumers(shard_count);
    for (std::uint32_t shard = 0; shard < shard_count; ++shard) {
        if (!shard_consumers[shard].attach(shard_config.paths[shard])) return 5;
    }

    unsigned char payload[72];
    unsigned char output[72];
    sze_recovery::CanonicalEvent event;
    sze_recovery::CanonicalEvent received;
    Metrics global;
    global.publish_ns.reserve(count);
    global.consume_ns.reserve(count);
    global.total_ns.reserve(count);
    std::uint64_t begin = sze_recovery::monotonic_time_ns();
    for (std::uint64_t id = 1U; id <= count; ++id) {
        make_event(id, &event, payload, sizeof(payload));
        const std::uint64_t t0 = sze_recovery::monotonic_time_ns();
        if (!global_producer.publish(event, payload)) return 6;
        const std::uint64_t t1 = sze_recovery::monotonic_time_ns();
        const sze_recovery::RingReadStatus status = global_consumer.read(
            id, &received, output, sizeof(output));
        const std::uint64_t t2 = sze_recovery::monotonic_time_ns();
        if (status != sze_recovery::kRingReadOk) {
            ++global.overruns;
            return 7;
        }
        global.publish_ns.push_back(t1 - t0);
        global.consume_ns.push_back(t2 - t1);
        global.total_ns.push_back(t2 - t0);
    }
    global.elapsed_ns = sze_recovery::monotonic_time_ns() - begin;

    Metrics sharded;
    sharded.publish_ns.reserve(count);
    sharded.consume_ns.reserve(count);
    sharded.total_ns.reserve(count);
    std::vector<std::uint64_t> expected(shard_count, 0U);
    begin = sze_recovery::monotonic_time_ns();
    for (std::uint64_t id = 1U; id <= count; ++id) {
        make_event(id, &event, payload, sizeof(payload));
        const std::uint32_t symbol = static_cast<std::uint32_t>(id % 3000U);
        std::uint32_t shard = 0U;
        std::uint64_t shard_event = 0U;
        const std::uint64_t t0 = sze_recovery::monotonic_time_ns();
        if (!shard_producer.publish(symbol, event, payload,
                                    &shard, &shard_event)) return 8;
        const std::uint64_t t1 = sze_recovery::monotonic_time_ns();
        if (shard_event != ++expected[shard]) return 9;
        const sze_recovery::RingReadStatus status = shard_consumers[shard].read(
            shard_event, &received, output, sizeof(output));
        const std::uint64_t t2 = sze_recovery::monotonic_time_ns();
        if (status != sze_recovery::kRingReadOk) {
            ++sharded.overruns;
            return 10;
        }
        sharded.publish_ns.push_back(t1 - t0);
        sharded.consume_ns.push_back(t2 - t1);
        sharded.total_ns.push_back(t2 - t0);
    }
    sharded.elapsed_ns = sze_recovery::monotonic_time_ns() - begin;

    const double global_eps = static_cast<double>(count) * 1e9 /
        static_cast<double>(global.elapsed_ns);
    const double sharded_eps = static_cast<double>(count) * 1e9 /
        static_cast<double>(sharded.elapsed_ns);
    const std::int64_t delta_p99 = static_cast<std::int64_t>(
        percentile(sharded.total_ns, 0.99)) - static_cast<std::int64_t>(
        percentile(global.total_ns, 0.99));
    const std::int64_t delta_p999 = static_cast<std::int64_t>(
        percentile(sharded.total_ns, 0.999)) - static_cast<std::int64_t>(
        percentile(global.total_ns, 0.999));
    const bool latency_pass = delta_p99 <= 1000 && delta_p999 <= 3000;
    const bool load_pass = sharded_eps >= 1.5 * peak_events_per_second &&
        sharded.overruns == 0U;

    std::cout << "sze_sharded_ring_benchmark synthetic=1 events=" << count
              << " shards=" << shard_count << '\n';
    print_metric("global", "publish", global.publish_ns);
    print_metric("global", "consume", global.consume_ns);
    print_metric("global", "receive_to_consume", global.total_ns);
    print_metric("sharded", "publish", sharded.publish_ns);
    print_metric("sharded", "consume", sharded.consume_ns);
    print_metric("sharded", "receive_to_consume", sharded.total_ns);
    std::cout << std::fixed << std::setprecision(0)
              << "summary global_events_per_second=" << global_eps
              << " sharded_events_per_second=" << sharded_eps
              << " delta_p99_ns=" << delta_p99
              << " delta_p99_9_ns=" << delta_p999
              << " global_overruns=" << global.overruns
              << " sharded_overruns=" << sharded.overruns
              << " latency_gate=" << (latency_pass ? "PASS" : "FAIL")
              << " load_1_5x_gate=" << (load_pass ? "PASS" : "FAIL")
              << " result=" << ((latency_pass && load_pass) ? "PASS" : "FAIL")
              << std::endl;

    global_consumer.close();
    global_producer.close();
    shard_consumers.clear();
    shard_producer.close();
    ::unlink(global_path);
    for (std::size_t i = 0; i < shard_config.paths.size(); ++i) {
        ::unlink(shard_config.paths[i].c_str());
    }
    return enforce && (!latency_pass || !load_pass) ? 11 : 0;
}
