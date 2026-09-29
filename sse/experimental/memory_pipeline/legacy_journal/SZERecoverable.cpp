#include "SZERecoverable.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <limits>

namespace sze_recovery {
namespace {

const char kJournalMagic[8] = {'S', 'Z', 'E', 'J', 'R', 'N', 'L', '1'};
const char kRingMagic[8] = {'S', 'Z', 'E', 'S', 'H', 'M', '0', '1'};
const std::uint32_t kRecordMagic = 0x31525a53U;
const std::uint64_t kCommitMagic = 0x3154494d4d4f4353ULL;

template <typename T>
T atomic_load_acquire(const T* value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

template <typename T>
void atomic_store_release(T* destination, T value)
{
    __atomic_store_n(destination, value, __ATOMIC_RELEASE);
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

bool file_exists(const std::string& path)
{
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool ensure_directory(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    std::string current;
    if (path[0] == '/') {
        current = "/";
    }
    std::size_t begin = path[0] == '/' ? 1U : 0U;
    while (begin <= path.size()) {
        const std::size_t slash = path.find('/', begin);
        const std::string component = path.substr(
            begin, slash == std::string::npos ? std::string::npos : slash - begin);
        if (!component.empty()) {
            if (!current.empty() && current[current.size() - 1U] != '/') {
                current.push_back('/');
            }
            current += component;
            if (::mkdir(current.c_str(), 0750) != 0 && errno != EEXIST) {
                return false;
            }
            struct stat info;
            if (::stat(current.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        begin = slash + 1U;
    }
    return true;
}

bool ensure_parent_directory(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return true;
    }
    if (slash == 0U) {
        return true;
    }
    return ensure_directory(path.substr(0, slash));
}

std::string segment_path_for(const JournalConfig& config, std::uint32_t index)
{
    char suffix[96];
    std::snprintf(suffix, sizeof(suffix), "%s_%08u_s%u_%06u.szej",
                  config.prefix.empty() ? "sze" : config.prefix.c_str(),
                  config.trading_day, config.source_id, index);
    std::string path = config.directory;
    if (!path.empty() && path[path.size() - 1U] != '/') {
        path.push_back('/');
    }
    path += suffix;
    return path;
}

bool valid_journal_config(const JournalConfig& config)
{
    const std::uint64_t minimum = kPageBytes + sizeof(JournalRecordHeader) +
        sizeof(JournalRecordTrailer) + 8U;
    return !config.directory.empty() && config.trading_day >= 20000101U &&
        config.trading_day <= 99991231U && config.source_id <= 65535U &&
        config.segment_bytes >= minimum &&
        config.segment_bytes <= static_cast<std::uint64_t>(SIZE_MAX) &&
        config.max_payload_bytes > 0U && config.max_payload_bytes <= 65535U;
}

bool valid_superblock(const JournalSuperblock* block,
                      const JournalConfig& config,
                      std::uint32_t expected_index)
{
    return block != 0 && std::memcmp(block->magic, kJournalMagic, 8) == 0 &&
        block->version == kFormatVersion && block->header_bytes == kPageBytes &&
        block->trading_day == config.trading_day &&
        block->source_id == config.source_id &&
        block->segment_index == expected_index &&
        block->segment_bytes == config.segment_bytes;
}

enum RecordValidation {
    kRecordValid,
    kRecordUnused,
    kRecordInvalid,
};

RecordValidation validate_record(const unsigned char* mapping,
                                 std::uint64_t segment_bytes,
                                 std::uint64_t offset,
                                 std::uint32_t max_payload_bytes,
                                 JournalRecordHeader* output,
                                 std::uint64_t* next_offset)
{
    if (offset > segment_bytes ||
        segment_bytes - offset < sizeof(JournalRecordHeader)) {
        return kRecordUnused;
    }
    JournalRecordHeader header;
    std::memcpy(&header, mapping + offset, sizeof(header));
    if (header.magic == 0U) {
        return kRecordUnused;
    }
    if (header.magic != kRecordMagic || header.version != kFormatVersion ||
        header.header_bytes != sizeof(JournalRecordHeader) ||
        header.payload_bytes > max_payload_bytes ||
        header.payload_bytes > 65535U) {
        return kRecordInvalid;
    }
    const std::uint64_t minimum_bytes = sizeof(JournalRecordHeader) +
        header.payload_bytes + sizeof(JournalRecordTrailer);
    if (header.total_bytes < minimum_bytes || (header.total_bytes & 7U) != 0U ||
        header.total_bytes > segment_bytes - offset) {
        return kRecordInvalid;
    }

    const std::uint32_t saved_header_crc = header.header_crc32;
    header.header_crc32 = 0U;
    if (crc32(&header, sizeof(header)) != saved_header_crc) {
        return kRecordInvalid;
    }
    header.header_crc32 = saved_header_crc;

    const unsigned char* payload = mapping + offset + sizeof(header);
    if (crc32(payload, header.payload_bytes) != header.payload_crc32) {
        return kRecordInvalid;
    }
    const JournalRecordTrailer* trailer = reinterpret_cast<const JournalRecordTrailer*>(
        mapping + offset + header.total_bytes - sizeof(JournalRecordTrailer));
    const std::uint64_t commit = atomic_load_acquire(&trailer->commit_magic);
    if (commit != kCommitMagic || trailer->event_id != header.event_id) {
        return kRecordInvalid;
    }
    if (output != 0) {
        *output = header;
    }
    if (next_offset != 0) {
        *next_offset = offset + header.total_bytes;
    }
    return kRecordValid;
}

std::uint32_t slot_crc(const CanonicalEvent& event, const void* payload)
{
    std::uint32_t value = crc32(&event, sizeof(event));
    if (event.payload_size > 0U && payload != 0) {
        value ^= crc32(payload, event.payload_size);
    }
    return value;
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t bytes)
{
    struct CrcTable {
        CrcTable()
        {
        for (std::uint32_t i = 0; i < 256U; ++i) {
            std::uint32_t value = i;
            for (unsigned bit = 0; bit < 8U; ++bit) {
                value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
            }
            table[i] = value;
        }
        }
        std::uint32_t table[256];
    };
    static const CrcTable crc_table;
    const unsigned char* cursor = static_cast<const unsigned char*>(data);
    std::uint32_t value = 0xffffffffU;
    for (std::size_t i = 0; i < bytes; ++i) {
        value = crc_table.table[(value ^ cursor[i]) & 0xffU] ^ (value >> 8U);
    }
    return value ^ 0xffffffffU;
}

std::uint64_t monotonic_time_ns()
{
    struct timespec value;
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0U;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(value.tv_nsec);
}

std::uint64_t realtime_ns()
{
    struct timespec value;
    if (::clock_gettime(CLOCK_REALTIME, &value) != 0) {
        return 0U;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(value.tv_nsec);
}

FeedSequenceTracker::FeedSequenceTracker()
    : trading_day_(0U),
      last_raw_sequence_(0U),
      wrap_base_(0U),
      last_sequence_(0U),
      duplicate_count_(0U),
      missing_count_(0U),
      state_(kContinuityInitializing),
      invalid_reason_(kInvalidNone),
      initialized_(false)
{
}

void FeedSequenceTracker::reset(std::uint32_t trading_day)
{
    trading_day_ = trading_day;
    last_raw_sequence_ = 0U;
    wrap_base_ = 0U;
    last_sequence_ = 0U;
    duplicate_count_ = 0U;
    missing_count_ = 0U;
    state_ = kContinuityInitializing;
    invalid_reason_ = kInvalidNone;
    initialized_ = false;
}

void FeedSequenceTracker::restore(std::uint32_t trading_day,
                                  std::uint64_t last_sequence,
                                  ContinuityState state,
                                  InvalidReason reason)
{
    reset(trading_day);
    if (last_sequence == 0U && state == kContinuityInitializing) {
        return;
    }
    initialized_ = true;
    last_sequence_ = last_sequence;
    last_raw_sequence_ = static_cast<std::uint32_t>(last_sequence & 0xffffffffULL);
    wrap_base_ = last_sequence & 0xffffffff00000000ULL;
    state_ = state;
    invalid_reason_ = state == kContinuityInvalid ? reason : kInvalidNone;
}

SequenceResult FeedSequenceTracker::observe(std::uint32_t raw_sequence)
{
    SequenceResult result;
    result.status = kSequenceAlreadyInvalid;
    result.sequence = last_sequence_;
    result.expected = initialized_ ? last_sequence_ + 1U : raw_sequence;
    result.missing = 0U;

    if (state_ == kContinuityInvalid) {
        const std::uint32_t delta = raw_sequence - last_raw_sequence_;
        if (raw_sequence != last_raw_sequence_ && delta < 0x80000000U) {
            std::uint64_t candidate_base = wrap_base_;
            if (raw_sequence < last_raw_sequence_ &&
                last_raw_sequence_ > 0xf0000000U && raw_sequence < 0x10000000U) {
                candidate_base += (1ULL << 32U);
            }
            wrap_base_ = candidate_base;
            last_raw_sequence_ = raw_sequence;
            last_sequence_ = candidate_base + raw_sequence;
            result.sequence = last_sequence_;
        }
        return result;
    }
    if (!initialized_) {
        initialized_ = true;
        last_raw_sequence_ = raw_sequence;
        last_sequence_ = raw_sequence;
        wrap_base_ = 0U;
        state_ = kContinuityValid;
        result.status = kSequenceFirst;
        result.sequence = last_sequence_;
        result.expected = last_sequence_;
        return result;
    }

    if (raw_sequence == last_raw_sequence_) {
        ++duplicate_count_;
        result.status = kSequenceDuplicate;
        result.sequence = last_sequence_;
        result.expected = last_sequence_ + 1U;
        return result;
    }

    const std::uint32_t expected_raw = last_raw_sequence_ + 1U;
    const std::uint32_t delta = raw_sequence - expected_raw;
    std::uint64_t candidate_base = wrap_base_;
    if (raw_sequence < last_raw_sequence_ &&
        last_raw_sequence_ > 0xf0000000U && raw_sequence < 0x10000000U) {
        candidate_base += (1ULL << 32U);
    }
    const std::uint64_t candidate = candidate_base + raw_sequence;
    result.sequence = candidate;
    result.expected = last_sequence_ + 1U;

    if (delta == 0U) {
        wrap_base_ = candidate_base;
        last_raw_sequence_ = raw_sequence;
        last_sequence_ = candidate;
        result.status = kSequenceAccepted;
        return result;
    }
    if (delta < 0x80000000U) {
        missing_count_ += delta;
        result.status = kSequenceGap;
        result.missing = delta;
        wrap_base_ = candidate_base;
        last_raw_sequence_ = raw_sequence;
        last_sequence_ = candidate;
        invalidate(kInvalidForwardGap);
        return result;
    }
    result.status = kSequenceRegression;
    invalidate(kInvalidRegression);
    return result;
}

void FeedSequenceTracker::invalidate(InvalidReason reason)
{
    if (state_ != kContinuityInvalid) {
        state_ = kContinuityInvalid;
        invalid_reason_ = reason;
    }
}

JournalConfig::JournalConfig()
    : directory(),
      prefix("sze"),
      trading_day(0U),
      source_id(88U),
      segment_bytes(1ULL << 30U),
      max_payload_bytes(static_cast<std::uint32_t>(kDefaultMaxPayloadBytes)),
      generation(0U),
      min_free_bytes_after_allocate(2ULL << 30U)
{
}

JournalWriter::JournalWriter()
    : config_(),
      fd_(-1),
      mapping_(0),
      superblock_(0),
      segment_index_(0U),
      write_offset_(kPageBytes),
      last_event_id_(0U),
      last_feed_sequence_(0U),
      flushed_offset_(kPageBytes),
      flush_count_(0U)
{
}

JournalWriter::~JournalWriter()
{
    if (is_open()) {
        close(false);
    }
}

std::string JournalWriter::segment_path(std::uint32_t index) const
{
    return segment_path_for(config_, index);
}

std::uint64_t JournalWriter::published_offset() const
{
    return superblock_
        ? atomic_load_acquire(&superblock_->published_offset) : 0U;
}

JournalOpenResult JournalWriter::open(const JournalConfig& config)
{
    JournalOpenResult result;
    if (is_open()) {
        close(false);
    }
    if (!valid_journal_config(config) || !ensure_directory(config.directory)) {
        return result;
    }
    config_ = config;

    std::uint32_t last_index = 0U;
    bool existing = file_exists(segment_path_for(config_, 0U));
    if (existing) {
        while (last_index != std::numeric_limits<std::uint32_t>::max() &&
               file_exists(segment_path_for(config_, last_index + 1U))) {
            ++last_index;
        }
    }

    JournalStatus status = kJournalOk;
    bool corrupt_tail = false;
    bool was_clean = true;
    if (existing) {
        status = open_existing_segment(last_index, &corrupt_tail, &was_clean);
    } else {
        if (config_.generation == 0U) {
            config_.generation = realtime_ns() ^ static_cast<std::uint64_t>(::getpid());
        }
        status = create_segment(0U, 1U);
    }
    if (status != kJournalOk) {
        unmap_segment();
        result.status = status;
        return result;
    }

    result.status = kJournalOk;
    result.existing = existing;
    result.unclean_restart = existing && !was_clean;
    result.corrupt_tail = corrupt_tail;
    result.last_event_id = last_event_id_;
    result.last_feed_sequence = last_feed_sequence_.load(std::memory_order_acquire);

    if (result.unclean_restart || corrupt_tail) {
        atomic_store_release(&superblock_->continuity_state,
            static_cast<std::uint32_t>(kContinuityInvalid));
        atomic_store_release(&superblock_->invalid_reason,
            static_cast<std::uint32_t>(corrupt_tail
                ? kInvalidJournalCorruption : kInvalidUncleanRestart));
    }
    result.continuity_state = static_cast<ContinuityState>(
        atomic_load_acquire(&superblock_->continuity_state));
    result.invalid_reason = static_cast<InvalidReason>(
        atomic_load_acquire(&superblock_->invalid_reason));
    atomic_store_release(&superblock_->clean_shutdown, 0U);
    if (::msync(mapping_, kPageBytes, MS_SYNC) != 0 || ::fdatasync(fd_) != 0) {
        result.status = kJournalIoError;
        unmap_segment();
    }
    return result;
}

JournalStatus JournalWriter::create_segment(std::uint32_t index,
                                            std::uint64_t first_event_id)
{
    const std::string path = segment_path_for(config_, index);
    struct statvfs filesystem;
    if (::statvfs(config_.directory.c_str(), &filesystem) != 0) {
        return kJournalIoError;
    }
    const std::uint64_t available =
        static_cast<std::uint64_t>(filesystem.f_bavail) *
        static_cast<std::uint64_t>(filesystem.f_frsize);
    if (available < config_.segment_bytes ||
        available - config_.segment_bytes <
            config_.min_free_bytes_after_allocate) {
        return kJournalIoError;
    }
    const int new_fd = ::open(
        path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    if (new_fd < 0) {
        return kJournalIoError;
    }
    const int allocation = ::posix_fallocate(new_fd, 0,
        static_cast<off_t>(config_.segment_bytes));
    if (allocation != 0 &&
        ::ftruncate(new_fd, static_cast<off_t>(config_.segment_bytes)) != 0) {
        ::close(new_fd);
        (void)::unlink(path.c_str());
        return kJournalIoError;
    }
    void* memory = ::mmap(0, static_cast<std::size_t>(config_.segment_bytes),
                          PROT_READ | PROT_WRITE, MAP_SHARED, new_fd, 0);
    if (memory == MAP_FAILED) {
        ::close(new_fd);
        (void)::unlink(path.c_str());
        return kJournalIoError;
    }
    unsigned char* new_mapping = static_cast<unsigned char*>(memory);
    JournalSuperblock* new_superblock =
        reinterpret_cast<JournalSuperblock*>(new_mapping);
    std::memset(new_superblock, 0, kPageBytes);
    std::memcpy(new_superblock->magic, kJournalMagic, 8);
    new_superblock->version = kFormatVersion;
    new_superblock->header_bytes = kPageBytes;
    new_superblock->trading_day = config_.trading_day;
    new_superblock->source_id = config_.source_id;
    new_superblock->segment_index = index;
    new_superblock->continuity_state = kContinuityInitializing;
    new_superblock->segment_bytes = config_.segment_bytes;
    new_superblock->generation = config_.generation;
    new_superblock->created_unix_ns = realtime_ns();
    new_superblock->first_event_id = first_event_id;
    new_superblock->last_committed_event_id =
        first_event_id > 0U ? first_event_id - 1U : 0U;
    new_superblock->published_offset = kPageBytes;
    new_superblock->flushed_offset = kPageBytes;
    new_superblock->clean_shutdown = 0U;
    new_superblock->invalid_reason = kInvalidNone;
    if (::msync(new_mapping, kPageBytes, MS_SYNC) != 0 ||
        ::fdatasync(new_fd) != 0) {
        ::munmap(new_mapping, static_cast<std::size_t>(config_.segment_bytes));
        ::close(new_fd);
        (void)::unlink(path.c_str());
        return kJournalIoError;
    }

    // Keep the old segment mapped until the replacement is fully allocated,
    // initialized, and durable. Failed rotation remains retryable.
    unmap_segment();
    fd_ = new_fd;
    mapping_ = new_mapping;
    superblock_ = new_superblock;
    segment_index_ = index;
    write_offset_ = kPageBytes;
    flushed_offset_ = kPageBytes;
    last_event_id_ = new_superblock->last_committed_event_id;
    return kJournalOk;
}

JournalStatus JournalWriter::open_existing_segment(std::uint32_t index,
                                                   bool* corrupt_tail,
                                                   bool* clean_shutdown)
{
    const std::string path = segment_path_for(config_, index);
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        return kJournalIoError;
    }
    struct stat info;
    if (::fstat(fd_, &info) != 0 ||
        static_cast<std::uint64_t>(info.st_size) != config_.segment_bytes) {
        ::close(fd_);
        fd_ = -1;
        return kJournalFormatError;
    }
    void* memory = ::mmap(0, static_cast<std::size_t>(config_.segment_bytes),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (memory == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        return kJournalIoError;
    }
    mapping_ = static_cast<unsigned char*>(memory);
    superblock_ = reinterpret_cast<JournalSuperblock*>(mapping_);
    if (!valid_superblock(superblock_, config_, index)) {
        unmap_segment();
        return kJournalFormatError;
    }
    if (clean_shutdown != 0) {
        *clean_shutdown = atomic_load_acquire(&superblock_->clean_shutdown) != 0U;
    }
    if (config_.generation != 0U &&
        config_.generation != superblock_->generation) {
        unmap_segment();
        return kJournalFormatError;
    }
    config_.generation = superblock_->generation;
    if (corrupt_tail != 0) {
        *corrupt_tail = false;
    }

    const std::uint64_t published = atomic_load_acquire(&superblock_->published_offset);
    if (published < kPageBytes || published > config_.segment_bytes) {
        unmap_segment();
        return kJournalCorrupt;
    }
    std::uint64_t offset = kPageBytes;
    std::uint64_t expected = superblock_->first_event_id;
    std::uint64_t last_event = expected > 0U ? expected - 1U : 0U;
    while (offset < published) {
        JournalRecordHeader header;
        std::uint64_t next_offset = offset;
        const RecordValidation validation = validate_record(
            mapping_, published, offset, config_.max_payload_bytes,
            &header, &next_offset);
        if (validation != kRecordValid || header.event_id != expected) {
            if (corrupt_tail != 0) {
                *corrupt_tail = true;
            }
            break;
        }
        last_event = header.event_id;
        ++expected;
        offset = next_offset;
    }
    if (offset != published && corrupt_tail != 0) {
        *corrupt_tail = true;
    }
    segment_index_ = index;
    write_offset_ = offset;
    last_event_id_ = last_event;
    last_feed_sequence_.store(
        atomic_load_acquire(&superblock_->last_feed_sequence),
        std::memory_order_release);
    flushed_offset_.store(std::min(
        atomic_load_acquire(&superblock_->flushed_offset), write_offset_),
        std::memory_order_release);
    atomic_store_release(&superblock_->published_offset, write_offset_);
    atomic_store_release(&superblock_->last_committed_event_id, last_event_id_);
    return kJournalOk;
}

JournalStatus JournalWriter::append(CanonicalEvent* event, const void* payload)
{
    if (!is_open() || event == 0 ||
        event->payload_size > config_.max_payload_bytes ||
        (event->payload_size > 0U && payload == 0)) {
        return kJournalInvalidArgument;
    }
    const std::uint64_t next_event = last_event_id_ + 1U;
    if (event->event_id == 0U) {
        event->event_id = next_event;
    } else if (event->event_id != next_event) {
        return kJournalInvalidArgument;
    }
    event->trading_day = config_.trading_day;
    event->source_id = static_cast<std::uint16_t>(config_.source_id);
    event->payload_crc32 = crc32(payload, event->payload_size);

    const std::uint64_t body_bytes = sizeof(JournalRecordHeader) + event->payload_size;
    const std::uint64_t total_bytes = align_up(body_bytes, 8U) +
        sizeof(JournalRecordTrailer);
    if (total_bytes > config_.segment_bytes - kPageBytes) {
        return kJournalInvalidArgument;
    }
    const std::uint64_t usable_bytes = config_.segment_bytes - kPageBytes;
    const std::uint64_t rotation_offset =
        kPageBytes + (usable_bytes * 9U) / 10U;
    if (write_offset_ >= rotation_offset ||
        write_offset_ > config_.segment_bytes ||
        total_bytes > config_.segment_bytes - write_offset_) {
        const JournalStatus rotate_status = rotate();
        if (rotate_status != kJournalOk) {
            return rotate_status;
        }
    }

    JournalRecordHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = kRecordMagic;
    header.version = static_cast<std::uint16_t>(kFormatVersion);
    header.header_bytes = static_cast<std::uint16_t>(sizeof(header));
    header.total_bytes = static_cast<std::uint32_t>(total_bytes);
    header.payload_bytes = event->payload_size;
    header.event_id = event->event_id;
    header.feed_sequence = event->feed_sequence;
    header.channel_sequence = event->channel_sequence;
    header.receive_mono_ns = event->receive_mono_ns;
    header.exchange_time = event->exchange_time;
    header.payload_crc32 = event->payload_crc32;
    header.source_id = event->source_id;
    header.channel_number = event->channel_number;
    header.message_type = event->message_type;
    header.record_kind = event->record_kind;
    header.flags = static_cast<std::uint16_t>(event->flags & 0xffffU);
    header.header_crc32 = 0U;
    header.header_crc32 = crc32(&header, sizeof(header));

    unsigned char* destination = mapping_ + write_offset_;
    std::memcpy(destination, &header, sizeof(header));
    if (event->payload_size > 0U) {
        std::memcpy(destination + sizeof(header), payload, event->payload_size);
    }
    const std::uint64_t trailer_offset = total_bytes - sizeof(JournalRecordTrailer);
    if (trailer_offset > body_bytes) {
        std::memset(destination + body_bytes, 0,
                    static_cast<std::size_t>(trailer_offset - body_bytes));
    }
    JournalRecordTrailer* trailer = reinterpret_cast<JournalRecordTrailer*>(
        destination + trailer_offset);
    trailer->event_id = event->event_id;
    atomic_store_release(&trailer->commit_magic, kCommitMagic);

    write_offset_ += total_bytes;
    last_event_id_ = event->event_id;
    last_feed_sequence_.store(event->feed_sequence, std::memory_order_release);
    atomic_store_release(&superblock_->last_committed_event_id, last_event_id_);
    atomic_store_release(&superblock_->last_feed_sequence, event->feed_sequence);
    atomic_store_release(&superblock_->published_offset, write_offset_);
    return kJournalOk;
}

JournalStatus JournalWriter::publish_continuity(ContinuityState state,
                                                InvalidReason reason,
                                                std::uint64_t last_feed_sequence)
{
    if (!is_open()) {
        return kJournalInvalidArgument;
    }
    last_feed_sequence_.store(last_feed_sequence, std::memory_order_release);
    atomic_store_release(&superblock_->last_feed_sequence, last_feed_sequence);
    if (atomic_load_acquire(&superblock_->continuity_state) !=
            static_cast<std::uint32_t>(kContinuityInvalid)) {
        atomic_store_release(&superblock_->continuity_state,
            static_cast<std::uint32_t>(state));
        atomic_store_release(&superblock_->invalid_reason,
            static_cast<std::uint32_t>(reason));
    }
    return kJournalOk;
}

JournalStatus JournalWriter::flush(bool synchronous)
{
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    return flush_mapped(synchronous);
}

JournalStatus JournalWriter::flush_mapped(bool synchronous)
{
    if (!is_open()) {
        return kJournalInvalidArgument;
    }
    const int mode = synchronous ? MS_SYNC : MS_ASYNC;
    const std::uint64_t target =
        atomic_load_acquire(&superblock_->published_offset);
    if (::msync(mapping_, static_cast<std::size_t>(target), mode) != 0) {
        return kJournalIoError;
    }
    if (synchronous && ::fdatasync(fd_) != 0) {
        return kJournalIoError;
    }
    flushed_offset_.store(target, std::memory_order_release);
    flush_count_.fetch_add(1U, std::memory_order_relaxed);
    atomic_store_release(&superblock_->flushed_offset, target);
    return kJournalOk;
}

JournalStatus JournalWriter::rotate()
{
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    const ContinuityState state = static_cast<ContinuityState>(
        atomic_load_acquire(&superblock_->continuity_state));
    const InvalidReason reason = static_cast<InvalidReason>(
        atomic_load_acquire(&superblock_->invalid_reason));
    const std::uint64_t feed_sequence =
        last_feed_sequence_.load(std::memory_order_acquire);
    const std::uint32_t next_index = segment_index_ + 1U;
    const std::uint64_t first_event = last_event_id_ + 1U;
    const JournalStatus flush_status = flush_mapped(true);
    if (flush_status != kJournalOk) {
        return flush_status;
    }
    const JournalStatus create_status = create_segment(next_index, first_event);
    if (create_status != kJournalOk) {
        return create_status;
    }
    last_feed_sequence_.store(feed_sequence, std::memory_order_release);
    publish_continuity(state, reason, feed_sequence);
    return kJournalOk;
}

JournalStatus JournalWriter::close(bool clean_shutdown)
{
    if (!is_open()) {
        return kJournalOk;
    }
    JournalStatus status = kJournalOk;
    if (clean_shutdown) {
        status = flush(true);
        if (status == kJournalOk) {
            atomic_store_release(&superblock_->clean_shutdown, 1U);
            if (::msync(mapping_, kPageBytes, MS_SYNC) != 0 ||
                ::fdatasync(fd_) != 0) {
                status = kJournalIoError;
            }
        }
    }
    unmap_segment();
    return status;
}

void JournalWriter::unmap_segment()
{
    if (mapping_ != 0) {
        ::munmap(mapping_, static_cast<std::size_t>(config_.segment_bytes));
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    mapping_ = 0;
    superblock_ = 0;
}

JournalReader::JournalReader()
    : config_(),
      fd_(-1),
      mapping_(0),
      superblock_(0),
      segment_index_(0U),
      read_offset_(kPageBytes),
      next_event_id_(0U)
{
}

JournalReader::~JournalReader()
{
    close();
}

JournalOpenResult JournalReader::open(const JournalConfig& config)
{
    JournalOpenResult result;
    close();
    if (!valid_journal_config(config)) {
        return result;
    }
    config_ = config;
    const JournalStatus status = map_segment(0U);
    if (status != kJournalOk) {
        result.status = status;
        return result;
    }
    std::uint32_t last_index = 0U;
    while (last_index != std::numeric_limits<std::uint32_t>::max() &&
           file_exists(segment_path_for(config_, last_index + 1U))) {
        ++last_index;
    }
    JournalSuperblock last_block;
    std::memset(&last_block, 0, sizeof(last_block));
    const std::string last_path = segment_path_for(config_, last_index);
    const int last_fd = ::open(last_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (last_fd < 0 || ::pread(last_fd, &last_block, sizeof(last_block), 0) !=
            static_cast<ssize_t>(sizeof(last_block)) ||
        !valid_superblock(&last_block, config_, last_index)) {
        if (last_fd >= 0) {
            ::close(last_fd);
        }
        close();
        result.status = kJournalFormatError;
        return result;
    }
    ::close(last_fd);
    result.status = kJournalOk;
    result.existing = true;
    result.unclean_restart = atomic_load_acquire(&last_block.clean_shutdown) == 0U;
    result.last_event_id = atomic_load_acquire(&last_block.last_committed_event_id);
    result.last_feed_sequence = atomic_load_acquire(&last_block.last_feed_sequence);
    result.continuity_state = static_cast<ContinuityState>(
        atomic_load_acquire(&last_block.continuity_state));
    result.invalid_reason = static_cast<InvalidReason>(
        atomic_load_acquire(&last_block.invalid_reason));
    return result;
}

JournalStatus JournalReader::map_segment(std::uint32_t index)
{
    unmap_segment();
    const std::string path = segment_path_for(config_, index);
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        return errno == ENOENT ? kJournalEnd : kJournalIoError;
    }
    struct stat info;
    if (::fstat(fd_, &info) != 0 ||
        static_cast<std::uint64_t>(info.st_size) != config_.segment_bytes) {
        unmap_segment();
        return kJournalFormatError;
    }
    void* memory = ::mmap(0, static_cast<std::size_t>(config_.segment_bytes),
                          PROT_READ, MAP_SHARED, fd_, 0);
    if (memory == MAP_FAILED) {
        mapping_ = 0;
        unmap_segment();
        return kJournalIoError;
    }
    mapping_ = static_cast<const unsigned char*>(memory);
    superblock_ = reinterpret_cast<const JournalSuperblock*>(mapping_);
    if (!valid_superblock(superblock_, config_, index)) {
        unmap_segment();
        return kJournalFormatError;
    }
    segment_index_ = index;
    read_offset_ = kPageBytes;
    next_event_id_ = superblock_->first_event_id;
    return kJournalOk;
}

JournalStatus JournalReader::next(CanonicalEvent* event,
                                  void* payload,
                                  std::size_t payload_capacity)
{
    if (!mapping_ || event == 0) {
        return kJournalInvalidArgument;
    }
    for (;;) {
        const std::uint64_t published = atomic_load_acquire(&superblock_->published_offset);
        if (published < kPageBytes || published > config_.segment_bytes) {
            return kJournalCorrupt;
        }
        if (read_offset_ >= published) {
            if (file_exists(segment_path_for(config_, segment_index_ + 1U))) {
                const std::uint64_t expected_event_id = next_event_id_;
                const JournalStatus status = map_segment(segment_index_ + 1U);
                if (status != kJournalOk) {
                    return status;
                }
                if (next_event_id_ != expected_event_id) {
                    return kJournalCorrupt;
                }
                continue;
            }
            return atomic_load_acquire(&superblock_->clean_shutdown) != 0U
                ? kJournalEnd : kJournalWouldBlock;
        }

        JournalRecordHeader header;
        std::uint64_t next_offset = read_offset_;
        if (validate_record(mapping_, published, read_offset_,
                            config_.max_payload_bytes, &header, &next_offset) !=
            kRecordValid) {
            return kJournalCorrupt;
        }
        if (header.event_id != next_event_id_ ||
            header.payload_bytes > payload_capacity ||
            (header.payload_bytes > 0U && payload == 0)) {
            return header.event_id != next_event_id_
                ? kJournalCorrupt : kJournalInvalidArgument;
        }

        std::memset(event, 0, sizeof(*event));
        event->event_id = header.event_id;
        event->feed_sequence = header.feed_sequence;
        event->channel_sequence = header.channel_sequence;
        event->receive_mono_ns = header.receive_mono_ns;
        event->exchange_time = header.exchange_time;
        event->trading_day = config_.trading_day;
        event->payload_crc32 = header.payload_crc32;
        event->source_id = header.source_id;
        event->channel_number = header.channel_number;
        event->payload_size = static_cast<std::uint16_t>(header.payload_bytes);
        event->message_type = header.message_type;
        event->record_kind = header.record_kind;
        event->flags = header.flags;
        if (header.payload_bytes > 0U) {
            std::memcpy(payload, mapping_ + read_offset_ + sizeof(header),
                        header.payload_bytes);
        }
        read_offset_ = next_offset;
        ++next_event_id_;
        return kJournalOk;
    }
}

JournalStatus JournalReader::seek(std::uint64_t event_id)
{
    if (event_id == 0U || config_.directory.empty()) {
        return kJournalInvalidArgument;
    }
    std::uint32_t index = 0U;
    for (;;) {
        const JournalStatus map_status = map_segment(index);
        if (map_status != kJournalOk) {
            return map_status;
        }
        const std::uint64_t first = superblock_->first_event_id;
        const std::uint64_t last =
            atomic_load_acquire(&superblock_->last_committed_event_id);
        const std::uint64_t published =
            atomic_load_acquire(&superblock_->published_offset);
        if (published < kPageBytes || published > config_.segment_bytes ||
            event_id < first) {
            return kJournalCorrupt;
        }
        if (event_id > last) {
            if (file_exists(segment_path_for(config_, index + 1U))) {
                ++index;
                continue;
            }
            if (event_id == last + 1U) {
                read_offset_ = published;
                next_event_id_ = event_id;
                return kJournalOk;
            }
            return atomic_load_acquire(&superblock_->clean_shutdown) != 0U
                ? kJournalEnd : kJournalWouldBlock;
        }

        std::uint64_t offset = kPageBytes;
        while (offset < published) {
            JournalRecordHeader header;
            std::uint64_t next_offset = offset;
            if (validate_record(mapping_, published, offset,
                                config_.max_payload_bytes, &header,
                                &next_offset) != kRecordValid) {
                return kJournalCorrupt;
            }
            if (header.event_id == event_id) {
                read_offset_ = offset;
                next_event_id_ = event_id;
                return kJournalOk;
            }
            if (header.event_id > event_id) {
                return kJournalCorrupt;
            }
            offset = next_offset;
        }
        return kJournalCorrupt;
    }
}

JournalStatus JournalReader::close()
{
    unmap_segment();
    return kJournalOk;
}

void JournalReader::unmap_segment()
{
    if (mapping_ != 0) {
        ::munmap(const_cast<unsigned char*>(mapping_),
                 static_cast<std::size_t>(config_.segment_bytes));
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    mapping_ = 0;
    superblock_ = 0;
}

RingConfig::RingConfig()
    : path(),
      trading_day(0U),
      source_id(88U),
      capacity(65536U),
      max_payload_bytes(static_cast<std::uint32_t>(kDefaultMaxPayloadBytes)),
      generation(0U)
{
}

ShmEventRing::ShmEventRing()
    : fd_(-1), mapping_(0), mapping_bytes_(0U), header_(0), producer_(false)
{
}

ShmEventRing::~ShmEventRing()
{
    close();
}

bool ShmEventRing::create(const RingConfig& config)
{
    close();
    if (config.path.empty() || config.trading_day < 20000101U ||
        config.capacity < 2U || config.max_payload_bytes == 0U ||
        config.max_payload_bytes > 65535U || !ensure_parent_directory(config.path)) {
        return false;
    }
    const std::uint64_t slot_bytes = align_up(
        sizeof(ShmSlotPrefix) + config.max_payload_bytes, 64U);
    const std::uint64_t total_bytes = kPageBytes +
        slot_bytes * static_cast<std::uint64_t>(config.capacity);
    if (total_bytes > static_cast<std::uint64_t>(SIZE_MAX) ||
        slot_bytes > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    fd_ = ::open(config.path.c_str(),
                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    if (fd_ < 0 || ::ftruncate(fd_, static_cast<off_t>(total_bytes)) != 0) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
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
    header_ = reinterpret_cast<ShmRingHeader*>(mapping_);
    producer_ = true;

    // The producer is expected to bind before create(); touching all pages here
    // makes the ring allocation deterministic on that NUMA node.
    std::memset(mapping_, 0, mapping_bytes_);
    std::memcpy(header_->magic, kRingMagic, 8);
    header_->version = kFormatVersion;
    header_->header_bytes = kPageBytes;
    header_->trading_day = config.trading_day;
    header_->source_id = config.source_id;
    header_->capacity = config.capacity;
    header_->max_payload_bytes = config.max_payload_bytes;
    header_->slot_bytes = static_cast<std::uint32_t>(slot_bytes);
    header_->continuity_state = kContinuityInitializing;
    header_->readiness_state = kReadinessNotReady;
    header_->generation = config.generation != 0U
        ? config.generation
        : (realtime_ns() ^ static_cast<std::uint64_t>(::getpid()));
    header_->producer_pid = static_cast<std::uint32_t>(::getpid());
    return true;
}

bool ShmEventRing::attach(const std::string& path)
{
    close();
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        return false;
    }
    struct stat info;
    if (::fstat(fd_, &info) != 0 || info.st_size < static_cast<off_t>(kPageBytes)) {
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
    header_ = reinterpret_cast<ShmRingHeader*>(mapping_);
    producer_ = false;
    if (std::memcmp(header_->magic, kRingMagic, 8) != 0 ||
        header_->version != kFormatVersion || header_->header_bytes != kPageBytes ||
        header_->capacity < 2U || header_->slot_bytes < sizeof(ShmSlotPrefix) ||
        header_->max_payload_bytes > header_->slot_bytes - sizeof(ShmSlotPrefix) ||
        static_cast<std::uint64_t>(kPageBytes) +
            static_cast<std::uint64_t>(header_->capacity) * header_->slot_bytes !=
            mapping_bytes_) {
        close();
        return false;
    }
    return true;
}

void ShmEventRing::close()
{
    if (mapping_ != 0) {
        ::munmap(mapping_, mapping_bytes_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    mapping_ = 0;
    mapping_bytes_ = 0U;
    header_ = 0;
    producer_ = false;
}

ShmSlotPrefix* ShmEventRing::slot(std::uint64_t event_id) const
{
    if (header_ == 0 || event_id == 0U) {
        return 0;
    }
    const std::uint64_t index = (event_id - 1U) % header_->capacity;
    return reinterpret_cast<ShmSlotPrefix*>(
        mapping_ + kPageBytes + index * header_->slot_bytes);
}

bool ShmEventRing::publish(const CanonicalEvent& event, const void* payload)
{
    if (!producer_ || event.event_id == 0U ||
        event.payload_size > header_->max_payload_bytes ||
        (event.payload_size > 0U && payload == 0)) {
        return false;
    }
    const std::uint64_t latest = atomic_load_acquire(&header_->latest_event_id);
    if (latest != 0U && event.event_id != latest + 1U) {
        return false;
    }
    ShmSlotPrefix* destination = slot(event.event_id);
    atomic_store_release(&destination->published_event_id,
                         static_cast<std::uint64_t>(0U));
    destination->generation = header_->generation;
    destination->event = event;
    unsigned char* payload_destination =
        reinterpret_cast<unsigned char*>(destination) + sizeof(ShmSlotPrefix);
    if (event.payload_size > 0U) {
        std::memcpy(payload_destination, payload, event.payload_size);
    }
    destination->slot_crc32 = slot_crc(event, payload_destination);
    atomic_store_release(&destination->published_event_id, event.event_id);
    atomic_store_release(&header_->latest_feed_sequence, event.feed_sequence);
    __atomic_add_fetch(&header_->publish_count,
                       static_cast<std::uint64_t>(1U), __ATOMIC_RELAXED);
    atomic_store_release(&header_->latest_event_id, event.event_id);
    return true;
}

RingReadStatus ShmEventRing::read(std::uint64_t expected_event_id,
                                  CanonicalEvent* event,
                                  void* payload,
                                  std::size_t payload_capacity) const
{
    if (!header_ || expected_event_id == 0U || event == 0) {
        return kRingReadInvalid;
    }
    const std::uint64_t latest = atomic_load_acquire(&header_->latest_event_id);
    if (expected_event_id > latest) {
        return kRingReadNotReady;
    }
    if (latest - expected_event_id >= header_->capacity) {
        return kRingReadOverrun;
    }
    const ShmSlotPrefix* source = slot(expected_event_id);
    const std::uint64_t published = atomic_load_acquire(&source->published_event_id);
    if (published != expected_event_id) {
        return published == 0U ? kRingReadOverrun : kRingReadOverrun;
    }
    if (source->generation != header_->generation) {
        return kRingReadStaleGeneration;
    }
    CanonicalEvent local_event = source->event;
    if (local_event.event_id != expected_event_id ||
        local_event.payload_size > header_->max_payload_bytes ||
        local_event.payload_size > payload_capacity ||
        (local_event.payload_size > 0U && payload == 0)) {
        return kRingReadInvalid;
    }
    const unsigned char* payload_source =
        reinterpret_cast<const unsigned char*>(source) + sizeof(ShmSlotPrefix);
    if (local_event.payload_size > 0U) {
        std::memcpy(payload, payload_source, local_event.payload_size);
    }
    if (slot_crc(local_event, payload) != source->slot_crc32 ||
        atomic_load_acquire(&source->published_event_id) != expected_event_id) {
        return kRingReadOverrun;
    }
    *event = local_event;
    return kRingReadOk;
}

void ShmEventRing::publish_state(ContinuityState continuity,
                                 ReadinessState readiness,
                                 InvalidReason reason,
                                 std::uint64_t invalid_event_id,
                                 std::uint64_t latest_feed_sequence)
{
    if (!producer_ || !header_) {
        return;
    }
    const std::uint32_t current = atomic_load_acquire(&header_->continuity_state);
    if (current != static_cast<std::uint32_t>(kContinuityInvalid)) {
        atomic_store_release(&header_->continuity_state,
                             static_cast<std::uint32_t>(continuity));
        atomic_store_release(&header_->invalid_reason,
                             static_cast<std::uint32_t>(reason));
        atomic_store_release(&header_->invalid_event_id, invalid_event_id);
    }
    if (continuity == kContinuityInvalid ||
        atomic_load_acquire(&header_->continuity_state) ==
            static_cast<std::uint32_t>(kContinuityInvalid)) {
        readiness = kReadinessNotReady;
    }
    atomic_store_release(&header_->latest_feed_sequence, latest_feed_sequence);
    atomic_store_release(&header_->readiness_state,
                         static_cast<std::uint32_t>(readiness));
}

void ShmEventRing::publish_continuity(ContinuityState continuity,
                                      InvalidReason reason,
                                      std::uint64_t invalid_event_id,
                                      std::uint64_t latest_feed_sequence)
{
    if (!producer_ || !header_) {
        return;
    }
    const std::uint32_t current = atomic_load_acquire(&header_->continuity_state);
    if (current != static_cast<std::uint32_t>(kContinuityInvalid)) {
        atomic_store_release(&header_->continuity_state,
                             static_cast<std::uint32_t>(continuity));
        atomic_store_release(&header_->invalid_reason,
                             static_cast<std::uint32_t>(reason));
        atomic_store_release(&header_->invalid_event_id, invalid_event_id);
    }
    atomic_store_release(&header_->latest_feed_sequence, latest_feed_sequence);
    if (continuity == kContinuityInvalid ||
        atomic_load_acquire(&header_->continuity_state) ==
            static_cast<std::uint32_t>(kContinuityInvalid)) {
        atomic_store_release(&header_->readiness_state,
            static_cast<std::uint32_t>(kReadinessNotReady));
    }
}

void ShmEventRing::set_readiness(ReadinessState readiness)
{
    if (!header_) {
        return;
    }
    if (atomic_load_acquire(&header_->continuity_state) ==
            static_cast<std::uint32_t>(kContinuityInvalid)) {
        readiness = kReadinessNotReady;
    }
    atomic_store_release(&header_->readiness_state,
                         static_cast<std::uint32_t>(readiness));
}

bool ShmEventRing::producer_alive() const
{
    if (!header_) {
        return false;
    }
    const pid_t producer = static_cast<pid_t>(
        atomic_load_acquire(&header_->producer_pid));
    return producer > 0 && (::kill(producer, 0) == 0 || errno == EPERM);
}

void ShmEventRing::publish_capture_metrics(std::uint64_t capture_records,
                                           std::uint64_t selected_records,
                                           std::uint64_t duplicate_records,
                                           std::uint64_t missing_records,
                                           std::uint64_t malformed_records)
{
    if (!producer_ || !header_) {
        return;
    }
    atomic_store_release(&header_->capture_records, capture_records);
    atomic_store_release(&header_->selected_records, selected_records);
    atomic_store_release(&header_->duplicate_records, duplicate_records);
    atomic_store_release(&header_->missing_records, missing_records);
    atomic_store_release(&header_->malformed_records, malformed_records);
}

void ShmEventRing::publish_storage_metrics(std::uint64_t journal_events,
                                           std::uint64_t journal_errors,
                                           std::uint64_t flush_count,
                                           std::uint64_t published_offset,
                                           std::uint64_t flushed_offset)
{
    if (!producer_ || !header_) {
        return;
    }
    atomic_store_release(&header_->journal_events, journal_events);
    atomic_store_release(&header_->journal_errors, journal_errors);
    atomic_store_release(&header_->flush_count, flush_count);
    atomic_store_release(&header_->journal_published_offset, published_offset);
    atomic_store_release(&header_->journal_flushed_offset, flushed_offset);
}

void ShmEventRing::set_journal_degraded(bool degraded)
{
    if (!producer_ || !header_) {
        return;
    }
    std::uint32_t flags = atomic_load_acquire(&header_->flags);
    if (degraded) {
        flags |= kRingFlagJournalDegraded;
    } else {
        flags &= ~kRingFlagJournalDegraded;
    }
    atomic_store_release(&header_->flags, flags);
}

void ShmEventRing::publish_replay_metrics(std::uint64_t replay_event_id,
                                          std::uint64_t replay_lag,
                                          std::uint64_t replay_rate_milli,
                                          std::uint64_t handoff_retries,
                                          std::uint64_t ring_overruns,
                                          std::uint64_t recovery_elapsed_ms)
{
    if (!header_) {
        return;
    }
    atomic_store_release(&header_->replay_event_id, replay_event_id);
    atomic_store_release(&header_->replay_lag, replay_lag);
    atomic_store_release(&header_->replay_rate_milli, replay_rate_milli);
    atomic_store_release(&header_->handoff_retries, handoff_retries);
    atomic_store_release(&header_->ring_overruns, ring_overruns);
    atomic_store_release(&header_->recovery_elapsed_ms, recovery_elapsed_ms);
}

std::uint64_t ShmEventRing::generation() const
{
    return header_ ? atomic_load_acquire(&header_->generation) : 0U;
}

std::uint64_t ShmEventRing::latest_event_id() const
{
    return header_ ? atomic_load_acquire(&header_->latest_event_id) : 0U;
}

std::uint64_t ShmEventRing::latest_feed_sequence() const
{
    return header_ ? atomic_load_acquire(&header_->latest_feed_sequence) : 0U;
}

ContinuityState ShmEventRing::continuity_state() const
{
    return header_ ? static_cast<ContinuityState>(
        atomic_load_acquire(&header_->continuity_state)) : kContinuityInvalid;
}

ReadinessState ShmEventRing::readiness_state() const
{
    return header_ ? static_cast<ReadinessState>(
        atomic_load_acquire(&header_->readiness_state)) : kReadinessNotReady;
}

ReplayHandoffConsumer::ReplayHandoffConsumer()
    : reader_(), ring_(), mode_(kReplayInvalid),
      last_open_status_(kReplayOpenNotAttempted), next_event_id_(0U),
      generation_(0U), replayed_events_(0U), live_events_(0U),
      handoff_retries_(0U), ring_overruns_(0U),
      publish_legacy_shared_readiness_(true)
{
}

ReplayHandoffConsumer::~ReplayHandoffConsumer()
{
    close();
}

bool ReplayHandoffConsumer::open(const JournalConfig& journal_config,
                                 const std::string& shm_path,
                                 bool publish_legacy_shared_readiness)
{
    close();
    last_open_status_ = kReplayOpenNotAttempted;
    const JournalOpenResult journal_open = reader_.open(journal_config);
    if (journal_open.status != kJournalOk) {
        close();
        last_open_status_ = kReplayOpenJournalFailed;
        return false;
    }
    if (!ring_.attach(shm_path)) {
        close();
        last_open_status_ = kReplayOpenShmFailed;
        return false;
    }
    const ShmRingHeader* ring_header = ring_.header();
    generation_ = reader_.generation();
    if (!ring_header || ring_header->trading_day != journal_config.trading_day ||
        ring_header->source_id != journal_config.source_id ||
        ring_.generation() != generation_) {
        close();
        last_open_status_ = kReplayOpenMetadataMismatch;
        return false;
    }
    next_event_id_ = reader_.next_event_id();
    replayed_events_ = 0U;
    live_events_ = 0U;
    handoff_retries_ = 0U;
    ring_overruns_ = 0U;
    publish_legacy_shared_readiness_ = publish_legacy_shared_readiness;
    if (ring_.continuity_state() == kContinuityInvalid) {
        mode_ = kReplayInvalid;
        if (publish_legacy_shared_readiness_) {
            ring_.set_readiness(kReadinessNotReady);
        }
    } else {
        mode_ = kReplayJournal;
        if (publish_legacy_shared_readiness_) {
            ring_.set_readiness(kReadinessReplaying);
        }
    }
    last_open_status_ = kReplayOpenOk;
    return true;
}

void ReplayHandoffConsumer::close()
{
    if (ring_.is_open() && publish_legacy_shared_readiness_) {
        ring_.set_readiness(kReadinessNotReady);
    }
    reader_.close();
    ring_.close();
    mode_ = kReplayInvalid;
    next_event_id_ = 0U;
    generation_ = 0U;
}

void ReplayHandoffConsumer::invalidate()
{
    mode_ = kReplayInvalid;
    if (publish_legacy_shared_readiness_) {
        ring_.set_readiness(kReadinessNotReady);
    }
}

ReplayReadStatus ReplayHandoffConsumer::read_ring(
    CanonicalEvent* event,
    void* payload,
    std::size_t payload_capacity)
{
    if (ring_.continuity_state() == kContinuityInvalid ||
        ring_.generation() != generation_ || !ring_.producer_alive()) {
        invalidate();
        return kReplayReadInvalid;
    }
    const RingReadStatus status = ring_.read(
        next_event_id_, event, payload, payload_capacity);
    if (status == kRingReadNotReady) {
        return kReplayReadWouldBlock;
    }
    if (status == kRingReadOverrun) {
        ++ring_overruns_;
        ++handoff_retries_;
        const JournalStatus seek_status = reader_.seek(next_event_id_);
        if (seek_status != kJournalOk) {
            invalidate();
            return kReplayReadError;
        }
        mode_ = kReplayJournal;
        if (publish_legacy_shared_readiness_) {
            ring_.set_readiness(kReadinessReplaying);
        }
        return kReplayReadWouldBlock;
    }
    if (status != kRingReadOk) {
        invalidate();
        return status == kRingReadStaleGeneration
            ? kReplayReadInvalid : kReplayReadError;
    }
    if (mode_ == kReplayHandoff) {
        mode_ = kReplayLive;
        if (publish_legacy_shared_readiness_) {
            ring_.set_readiness(kReadinessLiveReady);
        }
    }
    ++next_event_id_;
    ++live_events_;
    return kReplayReadEvent;
}

ReplayReadStatus ReplayHandoffConsumer::next(CanonicalEvent* event,
                                             void* payload,
                                             std::size_t payload_capacity)
{
    if (!event || mode_ == kReplayInvalid) {
        return kReplayReadInvalid;
    }
    if (ring_.continuity_state() == kContinuityInvalid ||
        ring_.generation() != generation_ || !ring_.producer_alive()) {
        invalidate();
        return kReplayReadInvalid;
    }
    if (mode_ == kReplayLive || mode_ == kReplayHandoff) {
        return read_ring(event, payload, payload_capacity);
    }

    const JournalStatus status = reader_.next(event, payload, payload_capacity);
    if (status == kJournalOk) {
        next_event_id_ = reader_.next_event_id();
        ++replayed_events_;
        return kReplayReadEvent;
    }
    if (status != kJournalWouldBlock && status != kJournalEnd) {
        invalidate();
        return status == kJournalCorrupt || status == kJournalFormatError
            ? kReplayReadInvalid : kReplayReadError;
    }

    const std::uint64_t latest = ring_.latest_event_id();
    if (next_event_id_ == latest + 1U) {
        mode_ = kReplayHandoff;
        if (publish_legacy_shared_readiness_) {
            ring_.set_readiness(kReadinessHandoff);
        }
        return read_ring(event, payload, payload_capacity);
    }
    return kReplayReadWouldBlock;
}

void ReplayHandoffConsumer::publish_metrics(std::uint64_t replay_rate_milli,
                                            std::uint64_t recovery_elapsed_ms)
{
    const std::uint64_t replay_event = next_event_id_ > 0U
        ? next_event_id_ - 1U : 0U;
    const std::uint64_t latest = ring_.latest_event_id();
    const std::uint64_t lag = latest > replay_event
        ? latest - replay_event : 0U;
    if (publish_legacy_shared_readiness_) {
        ring_.publish_replay_metrics(replay_event, lag, replay_rate_milli,
                                     handoff_retries_, ring_overruns_,
                                     recovery_elapsed_ms);
    }
}

}  // namespace sze_recovery
