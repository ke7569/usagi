#include "sze/market_data/SZEDirectShardedRing.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

int main()
{
    const std::uint32_t shard_count = 4U;
    sze_sharded_ring::DirectShardedRingConfig config;
    config.enabled = true;
    config.trading_day = 20260814U;
    config.source_id = 88U;
    config.shard_count = shard_count;
    config.capacity_per_shard = 64U;
    config.max_payload_bytes = 128U;
    config.generation = 44U;
    for (std::uint32_t shard = 0; shard < shard_count; ++shard) {
        char path[128];
        std::snprintf(path, sizeof(path), "/tmp/sze_shard_test_%ld_%u",
                      static_cast<long>(::getpid()), shard);
        config.paths.push_back(path);
    }
    config.assignments.push_back(std::make_pair(1U, 0U));
    config.assignments.push_back(std::make_pair(2U, 1U));
    config.assignments.push_back(std::make_pair(807U, 2U));

    sze_sharded_ring::DirectShardedRingProducer producer;
    assert(producer.create(config));
    assert(producer.route(1U) == 0U);
    assert(producer.route(807U) == 2U);
    assert(producer.route(999999U) == sze_sharded_ring::kUnroutedShard);

    sze_sharded_ring::ShardEventRing consumers[shard_count];
    for (std::uint32_t shard = 0; shard < shard_count; ++shard) {
        assert(consumers[shard].attach(config.paths[shard]));
    }
    unsigned char payload[72];
    std::memset(payload, 0x5a, sizeof(payload));
    sze_recovery::CanonicalEvent event;
    std::memset(&event, 0, sizeof(event));
    event.event_id = 900U;
    event.feed_sequence = 1000U;
    event.payload_size = sizeof(payload);
    std::uint32_t shard_id = 99U;
    std::uint64_t shard_event_id = 0U;
    assert(producer.publish(807U, event, payload, &shard_id, &shard_event_id));
    assert(shard_id == 2U);
    assert(shard_event_id == 1U);

    sze_recovery::CanonicalEvent output;
    unsigned char output_payload[72];
    assert(consumers[2].read(1U, &output, output_payload,
                             sizeof(output_payload)) == sze_recovery::kRingReadOk);
    assert(output.event_id == 900U);
    assert(output.feed_sequence == 1000U);
    assert(std::memcmp(payload, output_payload, sizeof(payload)) == 0);
    assert(consumers[0].read(1U, &output, output_payload,
                             sizeof(output_payload)) ==
           sze_recovery::kRingReadNotReady);
    assert(!producer.publish(999999U, event, payload, 0, 0));

    for (std::uint32_t shard = 0; shard < shard_count; ++shard) {
        consumers[shard].close();
    }
    producer.close();
    for (std::uint32_t shard = 0; shard < shard_count; ++shard) {
        ::unlink(config.paths[shard].c_str());
    }
    return 0;
}
