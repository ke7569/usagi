#include "sze/market_data/SZEDirectShardedRing.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace sze_sharded_ring {
namespace {

const char kMagic[8] = {'S','Z','E','S','H','R','0','1'};

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

template <typename T>
void store_release(T* destination, T value)
{
    __atomic_store_n(destination, value, __ATOMIC_RELEASE);
}

template <typename T>
T load_acquire(const T* source)
{
    return __atomic_load_n(source, __ATOMIC_ACQUIRE);
}

std::uint32_t event_crc(const sze_recovery::CanonicalEvent& event,
                        const void* payload)
{
    std::uint32_t crc = sze_recovery::crc32(&event, sizeof(event));
    if (event.payload_size > 0U) {
        crc ^= sze_recovery::crc32(payload, event.payload_size);
    }
    return crc;
}

}  // namespace

ShardRingConfig::ShardRingConfig()
    : trading_day(0U), source_id(0U), shard_id(0U), shard_count(0U),
      capacity(262144U), max_payload_bytes(256U), generation(0U) {}

ShardEventRing::ShardEventRing()
    : fd_(-1), mapping_(0), mapping_bytes_(0U), header_(0), producer_(false) {}
ShardEventRing::~ShardEventRing() { close(); }

bool ShardEventRing::create(const ShardRingConfig& config)
{
    close();
    if (config.path.empty() || config.trading_day < 20000101U ||
        config.shard_count == 0U || config.shard_id >= config.shard_count ||
        config.capacity < 2U || config.max_payload_bytes == 0U ||
        config.max_payload_bytes > 65535U) {
        return false;
    }
    const std::uint64_t slot_bytes = align_up(
        sizeof(ShardRingSlotPrefix) + config.max_payload_bytes, 64U);
    const std::uint64_t total_bytes = sze_recovery::kPageBytes +
        slot_bytes * config.capacity;
    if (total_bytes > static_cast<std::uint64_t>(SIZE_MAX) ||
        slot_bytes > std::numeric_limits<std::uint32_t>::max()) return false;
    fd_ = ::open(config.path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    if (fd_ < 0 || ::ftruncate(fd_, static_cast<off_t>(total_bytes)) != 0) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        return false;
    }
    void* memory = ::mmap(0, static_cast<std::size_t>(total_bytes),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (memory == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    mapping_ = static_cast<unsigned char*>(memory);
    mapping_bytes_ = static_cast<std::size_t>(total_bytes);
    std::memset(mapping_, 0, mapping_bytes_);
    header_ = reinterpret_cast<ShardRingHeader*>(mapping_);
    std::memcpy(header_->magic, kMagic, 8);
    header_->version = kShardRingFormatVersion;
    header_->header_bytes = sze_recovery::kPageBytes;
    header_->trading_day = config.trading_day;
    header_->source_id = config.source_id;
    header_->shard_id = config.shard_id;
    header_->shard_count = config.shard_count;
    header_->capacity = config.capacity;
    header_->max_payload_bytes = config.max_payload_bytes;
    header_->slot_bytes = static_cast<std::uint32_t>(slot_bytes);
    header_->generation = config.generation;
    header_->producer_pid = static_cast<std::uint32_t>(::getpid());
    producer_ = true;
    return true;
}

bool ShardEventRing::attach(const std::string& path)
{
    close();
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    struct stat info;
    if (fd_ < 0 || ::fstat(fd_, &info) != 0 ||
        info.st_size < static_cast<off_t>(sze_recovery::kPageBytes)) {
        close();
        return false;
    }
    mapping_bytes_ = static_cast<std::size_t>(info.st_size);
    void* memory = ::mmap(0, mapping_bytes_, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd_, 0);
    if (memory == MAP_FAILED) {
        mapping_ = 0;
        close();
        return false;
    }
    mapping_ = static_cast<unsigned char*>(memory);
    header_ = reinterpret_cast<ShardRingHeader*>(mapping_);
    const std::uint64_t expected = sze_recovery::kPageBytes +
        static_cast<std::uint64_t>(header_->capacity) * header_->slot_bytes;
    if (std::memcmp(header_->magic, kMagic, 8) != 0 ||
        header_->version != kShardRingFormatVersion ||
        header_->header_bytes != sze_recovery::kPageBytes ||
        header_->capacity < 2U ||
        header_->slot_bytes < sizeof(ShardRingSlotPrefix) ||
        expected != mapping_bytes_) {
        close();
        return false;
    }
    producer_ = false;
    return true;
}

void ShardEventRing::close()
{
    if (mapping_) ::munmap(mapping_, mapping_bytes_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    mapping_ = 0;
    mapping_bytes_ = 0U;
    header_ = 0;
    producer_ = false;
}

ShardRingSlotPrefix* ShardEventRing::slot(std::uint64_t shard_event_id) const
{
    if (!header_ || shard_event_id == 0U) return 0;
    const std::uint64_t index = (shard_event_id - 1U) % header_->capacity;
    return reinterpret_cast<ShardRingSlotPrefix*>(
        mapping_ + sze_recovery::kPageBytes + index * header_->slot_bytes);
}

bool ShardEventRing::publish(std::uint64_t shard_event_id,
                             const sze_recovery::CanonicalEvent& event,
                             const void* payload)
{
    if (!producer_ || !header_ || shard_event_id == 0U || event.event_id == 0U ||
        event.payload_size > header_->max_payload_bytes ||
        (event.payload_size > 0U && !payload)) return false;
    const std::uint64_t latest = load_acquire(&header_->latest_shard_event_id);
    if (shard_event_id != latest + 1U) return false;
    ShardRingSlotPrefix* destination = slot(shard_event_id);
    store_release(&destination->published_shard_event_id,
                  static_cast<std::uint64_t>(0U));
    destination->generation = header_->generation;
    destination->event = event;
    unsigned char* payload_destination = reinterpret_cast<unsigned char*>(destination) +
        sizeof(ShardRingSlotPrefix);
    if (event.payload_size > 0U) {
        std::memcpy(payload_destination, payload, event.payload_size);
    }
    destination->slot_crc32 = event_crc(event, payload_destination);
    store_release(&destination->published_shard_event_id, shard_event_id);
    store_release(&header_->latest_global_event_id, event.event_id);
    store_release(&header_->latest_feed_sequence, event.feed_sequence);
    __atomic_add_fetch(&header_->publish_count,
                       static_cast<std::uint64_t>(1U), __ATOMIC_RELAXED);
    store_release(&header_->latest_shard_event_id, shard_event_id);
    return true;
}

sze_recovery::RingReadStatus ShardEventRing::read(
    std::uint64_t expected_shard_event_id, sze_recovery::CanonicalEvent* event,
    void* payload, std::size_t payload_capacity) const
{
    if (!header_ || expected_shard_event_id == 0U || !event) {
        return sze_recovery::kRingReadInvalid;
    }
    const std::uint64_t latest = load_acquire(&header_->latest_shard_event_id);
    if (expected_shard_event_id > latest) return sze_recovery::kRingReadNotReady;
    if (latest - expected_shard_event_id >= header_->capacity) {
        return sze_recovery::kRingReadOverrun;
    }
    const ShardRingSlotPrefix* source = slot(expected_shard_event_id);
    if (load_acquire(&source->published_shard_event_id) != expected_shard_event_id) {
        return sze_recovery::kRingReadOverrun;
    }
    if (source->generation != header_->generation) {
        return sze_recovery::kRingReadStaleGeneration;
    }
    const sze_recovery::CanonicalEvent local = source->event;
    if (local.payload_size > payload_capacity ||
        (local.payload_size > 0U && !payload)) return sze_recovery::kRingReadInvalid;
    const unsigned char* payload_source = reinterpret_cast<const unsigned char*>(source) +
        sizeof(ShardRingSlotPrefix);
    if (local.payload_size > 0U) std::memcpy(payload, payload_source, local.payload_size);
    if (event_crc(local, payload) != source->slot_crc32 ||
        load_acquire(&source->published_shard_event_id) != expected_shard_event_id) {
        return sze_recovery::kRingReadOverrun;
    }
    *event = local;
    return sze_recovery::kRingReadOk;
}

DirectShardedRingConfig::DirectShardedRingConfig()
    : enabled(false), trading_day(0U), source_id(0U), shard_count(0U),
      capacity_per_shard(262144U), max_payload_bytes(256U), generation(0U) {}

DirectShardedRingProducer::DirectShardedRingProducer() {}
DirectShardedRingProducer::~DirectShardedRingProducer() { close(); }

bool DirectShardedRingProducer::create(const DirectShardedRingConfig& config)
{
    close();
    if (!config.enabled || config.shard_count == 0U || config.shard_count > 255U ||
        config.paths.size() != config.shard_count || config.assignments.empty()) {
        return false;
    }
    symbol_to_shard_.assign(1000000U, 0xffU);
    for (std::size_t i = 0; i < config.assignments.size(); ++i) {
        const std::uint32_t symbol = config.assignments[i].first;
        const std::uint32_t shard = config.assignments[i].second;
        if (symbol >= symbol_to_shard_.size() || shard >= config.shard_count ||
            symbol_to_shard_[symbol] != 0xffU) {
            close();
            return false;
        }
        symbol_to_shard_[symbol] = static_cast<std::uint8_t>(shard);
    }
    next_shard_event_id_.assign(config.shard_count, 0U);
    rings_.reserve(config.shard_count);
    std::vector<std::string> created_paths;
    for (std::uint32_t shard = 0U; shard < config.shard_count; ++shard) {
        std::unique_ptr<ShardEventRing> ring(new ShardEventRing());
        ShardRingConfig ring_config;
        ring_config.path = config.paths[shard];
        ring_config.trading_day = config.trading_day;
        ring_config.source_id = config.source_id;
        ring_config.shard_id = shard;
        ring_config.shard_count = config.shard_count;
        ring_config.capacity = config.capacity_per_shard;
        ring_config.max_payload_bytes = config.max_payload_bytes;
        ring_config.generation = config.generation;
        if (!ring->create(ring_config)) {
            close();
            for (std::size_t i = 0; i < created_paths.size(); ++i) {
                (void)::unlink(created_paths[i].c_str());
            }
            return false;
        }
        created_paths.push_back(ring_config.path);
        rings_.push_back(std::move(ring));
    }
    return true;
}

void DirectShardedRingProducer::close()
{
    rings_.clear();
    next_shard_event_id_.clear();
    symbol_to_shard_.clear();
}

std::uint32_t DirectShardedRingProducer::route(std::uint32_t symbol_id) const
{
    if (symbol_id >= symbol_to_shard_.size()) return kUnroutedShard;
    const std::uint8_t shard = symbol_to_shard_[symbol_id];
    return shard == 0xffU ? kUnroutedShard : shard;
}

bool DirectShardedRingProducer::publish(
    std::uint32_t symbol_id, const sze_recovery::CanonicalEvent& event,
    const void* payload, std::uint32_t* shard_id, std::uint64_t* shard_event_id)
{
    const std::uint32_t shard = route(symbol_id);
    if (shard == kUnroutedShard || shard >= rings_.size()) return false;
    const std::uint64_t sequence = ++next_shard_event_id_[shard];
    if (!rings_[shard]->publish(sequence, event, payload)) {
        --next_shard_event_id_[shard];
        return false;
    }
    if (shard_id) *shard_id = shard;
    if (shard_event_id) *shard_event_id = sequence;
    return true;
}

}  // namespace sze_sharded_ring
