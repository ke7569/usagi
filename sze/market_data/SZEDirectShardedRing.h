#ifndef DEEPWIN_SZE_DIRECT_SHARDED_RING_H
#define DEEPWIN_SZE_DIRECT_SHARDED_RING_H

#include "sze/market_data/SZERecoverable.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sze_sharded_ring {

static const std::uint32_t kShardRingFormatVersion = 1U;
static const std::uint32_t kUnroutedShard = 0xffffffffU;

struct alignas(64) ShardRingHeader {
    char magic[8];
    std::uint64_t generation;
    std::uint64_t latest_shard_event_id;
    std::uint64_t latest_global_event_id;
    std::uint64_t latest_feed_sequence;
    std::uint64_t publish_count;
    std::uint64_t overrun_count;
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint32_t shard_id;
    std::uint32_t shard_count;
    std::uint32_t capacity;
    std::uint32_t max_payload_bytes;
    std::uint32_t slot_bytes;
    std::uint32_t producer_pid;
    std::uint32_t flags;
    std::uint8_t reserved[3996];
};

struct alignas(64) ShardRingSlotPrefix {
    std::uint64_t published_shard_event_id;
    std::uint64_t generation;
    sze_recovery::CanonicalEvent event;
    std::uint32_t slot_crc32;
    std::uint8_t reserved[44];
};

static_assert(sizeof(ShardRingHeader) == sze_recovery::kPageBytes,
              "unexpected shard ring header ABI");
static_assert(sizeof(ShardRingSlotPrefix) == 128U,
              "unexpected shard ring slot ABI");

struct ShardRingConfig {
    ShardRingConfig();
    std::string path;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint32_t shard_id;
    std::uint32_t shard_count;
    std::uint32_t capacity;
    std::uint32_t max_payload_bytes;
    std::uint64_t generation;
};

class ShardEventRing {
public:
    ShardEventRing();
    ~ShardEventRing();
    bool create(const ShardRingConfig& config);
    bool attach(const std::string& path);
    void close();
    bool publish(std::uint64_t shard_event_id,
                 const sze_recovery::CanonicalEvent& event,
                 const void* payload);
    sze_recovery::RingReadStatus read(
        std::uint64_t expected_shard_event_id,
        sze_recovery::CanonicalEvent* event,
        void* payload, std::size_t payload_capacity) const;
    const ShardRingHeader* header() const { return header_; }
private:
    ShardRingSlotPrefix* slot(std::uint64_t shard_event_id) const;
    int fd_;
    unsigned char* mapping_;
    std::size_t mapping_bytes_;
    ShardRingHeader* header_;
    bool producer_;
};

struct DirectShardedRingConfig {
    DirectShardedRingConfig();
    bool enabled;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint32_t shard_count;
    std::uint32_t capacity_per_shard;
    std::uint32_t max_payload_bytes;
    std::uint64_t generation;
    std::vector<std::string> paths;
    std::vector<std::pair<std::uint32_t, std::uint32_t> > assignments;
};

class DirectShardedRingProducer {
public:
    DirectShardedRingProducer();
    ~DirectShardedRingProducer();
    bool create(const DirectShardedRingConfig& config);
    void close();
    bool publish(std::uint32_t symbol_id,
                 const sze_recovery::CanonicalEvent& event,
                 const void* payload,
                 std::uint32_t* shard_id,
                 std::uint64_t* shard_event_id);
    std::uint32_t route(std::uint32_t symbol_id) const;
    std::uint32_t shard_count() const {
        return static_cast<std::uint32_t>(rings_.size());
    }
private:
    std::vector<std::uint8_t> symbol_to_shard_;
    std::vector<std::uint64_t> next_shard_event_id_;
    std::vector<std::unique_ptr<ShardEventRing> > rings_;
};

}  // namespace sze_sharded_ring

#endif
