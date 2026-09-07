#ifndef DEEPWIN_SZE_RECOVERABLE_H
#define DEEPWIN_SZE_RECOVERABLE_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>

namespace sze_recovery {

static const std::uint32_t kFormatVersion = 1U;
static const std::size_t kPageBytes = 4096U;
static const std::size_t kDefaultMaxPayloadBytes = 256U;

enum ContinuityState {
    kContinuityInitializing = 0,
    kContinuityValid = 1,
    kContinuityInvalid = 2,
};

enum InvalidReason {
    kInvalidNone = 0,
    kInvalidForwardGap = 1,
    kInvalidRegression = 2,
    kInvalidMalformedRecord = 3,
    kInvalidJournalCorruption = 4,
    kInvalidUncleanRestart = 5,
    kInvalidRingOverrun = 6,
    kInvalidFormatMismatch = 7,
    kInvalidTradingDayMismatch = 8,
    kInvalidReceiverStopped = 9,
};

inline const char* invalid_reason_name(InvalidReason value)
{
    switch (value) {
    case kInvalidNone: return "none";
    case kInvalidForwardGap: return "forward_gap";
    case kInvalidRegression: return "regression";
    case kInvalidMalformedRecord: return "malformed_record";
    case kInvalidJournalCorruption: return "journal_corruption";
    case kInvalidUncleanRestart: return "unclean_restart";
    case kInvalidRingOverrun: return "ring_overrun";
    case kInvalidFormatMismatch: return "format_mismatch";
    case kInvalidTradingDayMismatch: return "trading_day_mismatch";
    case kInvalidReceiverStopped: return "receiver_stopped";
    }
    return "unknown";
}

enum ReadinessState {
    kReadinessNotReady = 0,
    kReadinessReplaying = 1,
    kReadinessHandoff = 2,
    kReadinessLiveReady = 3,
};

static const std::uint32_t kRingFlagJournalDegraded = 1U << 0U;

enum SequenceStatus {
    kSequenceFirst = 0,
    kSequenceAccepted = 1,
    kSequenceDuplicate = 2,
    kSequenceGap = 3,
    kSequenceRegression = 4,
    kSequenceAlreadyInvalid = 5,
};

enum RecordKind {
    kRecordMarketData = 1,
    kRecordContinuity = 2,
};

struct CanonicalEvent {
    std::uint64_t event_id;
    std::uint64_t feed_sequence;
    std::uint64_t channel_sequence;
    std::uint64_t receive_mono_ns;
    std::uint64_t exchange_time;
    std::uint32_t trading_day;
    std::uint32_t payload_crc32;
    std::uint16_t source_id;
    std::uint16_t channel_number;
    std::uint16_t payload_size;
    std::uint8_t message_type;
    std::uint8_t record_kind;
    std::uint32_t flags;
    std::uint32_t reserved;
};

struct alignas(64) JournalSuperblock {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint32_t segment_index;
    std::uint32_t continuity_state;
    std::uint64_t segment_bytes;
    std::uint64_t generation;
    std::uint64_t created_unix_ns;
    std::uint64_t first_event_id;
    std::uint64_t last_committed_event_id;
    std::uint64_t last_feed_sequence;
    std::uint64_t published_offset;
    std::uint64_t flushed_offset;
    std::uint32_t clean_shutdown;
    std::uint32_t invalid_reason;
    std::uint8_t reserved[3992];
};

struct JournalRecordHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t header_bytes;
    std::uint32_t total_bytes;
    std::uint32_t payload_bytes;
    std::uint64_t event_id;
    std::uint64_t feed_sequence;
    std::uint64_t channel_sequence;
    std::uint64_t receive_mono_ns;
    std::uint64_t exchange_time;
    std::uint32_t payload_crc32;
    std::uint16_t source_id;
    std::uint16_t channel_number;
    std::uint8_t message_type;
    std::uint8_t record_kind;
    std::uint16_t flags;
    std::uint32_t header_crc32;
};

struct JournalRecordTrailer {
    std::uint64_t event_id;
    std::uint64_t commit_magic;
};

struct alignas(64) ShmRingHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint32_t capacity;
    std::uint32_t max_payload_bytes;
    std::uint32_t slot_bytes;
    std::uint32_t continuity_state;
    std::uint32_t readiness_state;
    std::uint32_t flags;
    std::uint64_t generation;
    std::uint64_t latest_event_id;
    std::uint64_t latest_feed_sequence;
    std::uint64_t publish_count;
    std::uint64_t invalid_event_id;
    std::uint32_t invalid_reason;
    std::uint32_t producer_pid;
    std::uint64_t capture_records;
    std::uint64_t selected_records;
    std::uint64_t duplicate_records;
    std::uint64_t missing_records;
    std::uint64_t malformed_records;
    std::uint64_t journal_events;
    std::uint64_t journal_errors;
    std::uint64_t flush_count;
    std::uint64_t journal_published_offset;
    std::uint64_t journal_flushed_offset;
    std::uint64_t replay_event_id;
    std::uint64_t replay_lag;
    std::uint64_t replay_rate_milli;
    std::uint64_t handoff_retries;
    std::uint64_t ring_overruns;
    std::uint64_t recovery_elapsed_ms;
    std::uint8_t reserved[3872];
};

struct alignas(64) ShmSlotPrefix {
    std::uint64_t published_event_id;
    std::uint64_t generation;
    CanonicalEvent event;
    std::uint32_t slot_crc32;
    std::uint8_t reserved[44];
};

static_assert(sizeof(CanonicalEvent) == 64U, "unexpected canonical event ABI");
static_assert(sizeof(JournalSuperblock) == kPageBytes, "unexpected journal superblock ABI");
static_assert(sizeof(JournalRecordHeader) == 72U, "unexpected journal record header ABI");
static_assert(sizeof(JournalRecordTrailer) == 16U, "unexpected journal trailer ABI");
static_assert(sizeof(ShmRingHeader) == kPageBytes, "unexpected shm ring header ABI");
static_assert(sizeof(ShmSlotPrefix) == 128U, "unexpected shm slot prefix ABI");

struct SequenceResult {
    SequenceStatus status;
    std::uint64_t sequence;
    std::uint64_t expected;
    std::uint64_t missing;
};

class FeedSequenceTracker {
public:
    FeedSequenceTracker();

    void reset(std::uint32_t trading_day);
    void restore(std::uint32_t trading_day,
                 std::uint64_t last_sequence,
                 ContinuityState state,
                 InvalidReason reason);
    SequenceResult observe(std::uint32_t raw_sequence);
    void invalidate(InvalidReason reason);

    std::uint32_t trading_day() const { return trading_day_; }
    ContinuityState state() const { return state_; }
    InvalidReason invalid_reason() const { return invalid_reason_; }
    std::uint64_t last_sequence() const { return last_sequence_; }
    std::uint64_t duplicate_count() const { return duplicate_count_; }
    std::uint64_t missing_count() const { return missing_count_; }

private:
    std::uint32_t trading_day_;
    std::uint32_t last_raw_sequence_;
    std::uint64_t wrap_base_;
    std::uint64_t last_sequence_;
    std::uint64_t duplicate_count_;
    std::uint64_t missing_count_;
    ContinuityState state_;
    InvalidReason invalid_reason_;
    bool initialized_;
};

struct JournalConfig {
    JournalConfig();

    std::string directory;
    std::string prefix;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint64_t segment_bytes;
    std::uint32_t max_payload_bytes;
    std::uint64_t generation;
    std::uint64_t min_free_bytes_after_allocate;
};

enum JournalStatus {
    kJournalOk = 0,
    kJournalEnd = 1,
    kJournalWouldBlock = 2,
    kJournalInvalidArgument = 3,
    kJournalIoError = 4,
    kJournalFormatError = 5,
    kJournalCorrupt = 6,
};

struct JournalOpenResult {
    JournalOpenResult()
        : status(kJournalInvalidArgument), existing(false),
          unclean_restart(false), corrupt_tail(false), last_event_id(0),
          last_feed_sequence(0), continuity_state(kContinuityInitializing),
          invalid_reason(kInvalidNone)
    {
    }

    JournalStatus status;
    bool existing;
    bool unclean_restart;
    bool corrupt_tail;
    std::uint64_t last_event_id;
    std::uint64_t last_feed_sequence;
    ContinuityState continuity_state;
    InvalidReason invalid_reason;
};

class JournalWriter {
public:
    JournalWriter();
    ~JournalWriter();

    JournalOpenResult open(const JournalConfig& config);
    JournalStatus append(CanonicalEvent* event, const void* payload);
    JournalStatus publish_continuity(ContinuityState state,
                                     InvalidReason reason,
                                     std::uint64_t last_feed_sequence);
    JournalStatus flush(bool synchronous);
    JournalStatus close(bool clean_shutdown);

    bool is_open() const { return mapping_ != 0; }
    std::uint64_t last_event_id() const { return last_event_id_; }
    std::uint64_t generation() const {
        return superblock_ ? superblock_->generation : 0U;
    }
    std::uint64_t published_offset() const;
    std::uint64_t flushed_offset() const {
        return flushed_offset_.load(std::memory_order_acquire);
    }
    std::uint64_t flush_count() const {
        return flush_count_.load(std::memory_order_acquire);
    }
    std::uint32_t segment_index() const { return segment_index_; }
    std::string segment_path(std::uint32_t index) const;

private:
    JournalStatus create_segment(std::uint32_t index, std::uint64_t first_event_id);
    JournalStatus open_existing_segment(std::uint32_t index,
                                        bool* corrupt_tail,
                                        bool* clean_shutdown);
    JournalStatus rotate();
    JournalStatus flush_mapped(bool synchronous);
    void unmap_segment();

    JournalConfig config_;
    int fd_;
    unsigned char* mapping_;
    JournalSuperblock* superblock_;
    std::uint32_t segment_index_;
    std::uint64_t write_offset_;
    std::uint64_t last_event_id_;
    std::atomic<std::uint64_t> last_feed_sequence_;
    std::atomic<std::uint64_t> flushed_offset_;
    std::atomic<std::uint64_t> flush_count_;
    std::mutex mapping_mutex_;
};

class JournalReader {
public:
    JournalReader();
    ~JournalReader();

    JournalOpenResult open(const JournalConfig& config);
    JournalStatus next(CanonicalEvent* event,
                       void* payload,
                       std::size_t payload_capacity);
    JournalStatus seek(std::uint64_t event_id);
    JournalStatus close();

    std::uint64_t next_event_id() const { return next_event_id_; }
    std::uint32_t segment_index() const { return segment_index_; }
    std::uint64_t generation() const {
        return superblock_ ? superblock_->generation : 0U;
    }

private:
    JournalStatus map_segment(std::uint32_t index);
    void unmap_segment();

    JournalConfig config_;
    int fd_;
    const unsigned char* mapping_;
    const JournalSuperblock* superblock_;
    std::uint32_t segment_index_;
    std::uint64_t read_offset_;
    std::uint64_t next_event_id_;
};

enum RingReadStatus {
    kRingReadOk = 0,
    kRingReadNotReady = 1,
    kRingReadOverrun = 2,
    kRingReadStaleGeneration = 3,
    kRingReadInvalid = 4,
};

struct RingConfig {
    RingConfig();

    std::string path;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint32_t capacity;
    std::uint32_t max_payload_bytes;
    std::uint64_t generation;
};

class ShmEventRing {
public:
    ShmEventRing();
    ~ShmEventRing();

    bool create(const RingConfig& config);
    bool attach(const std::string& path);
    void close();

    bool publish(const CanonicalEvent& event, const void* payload);
    RingReadStatus read(std::uint64_t expected_event_id,
                        CanonicalEvent* event,
                        void* payload,
                        std::size_t payload_capacity) const;
    void publish_state(ContinuityState continuity,
                       ReadinessState readiness,
                       InvalidReason reason,
                       std::uint64_t invalid_event_id,
                       std::uint64_t latest_feed_sequence);
    void publish_continuity(ContinuityState continuity,
                            InvalidReason reason,
                            std::uint64_t invalid_event_id,
                            std::uint64_t latest_feed_sequence);
    void set_readiness(ReadinessState readiness);
    bool producer_alive() const;
    void publish_capture_metrics(std::uint64_t capture_records,
                                 std::uint64_t selected_records,
                                 std::uint64_t duplicate_records,
                                 std::uint64_t missing_records,
                                 std::uint64_t malformed_records);
    void publish_storage_metrics(std::uint64_t journal_events,
                                 std::uint64_t journal_errors,
                                 std::uint64_t flush_count,
                                 std::uint64_t published_offset,
                                 std::uint64_t flushed_offset);
    void set_journal_degraded(bool degraded);
    void publish_replay_metrics(std::uint64_t replay_event_id,
                                std::uint64_t replay_lag,
                                std::uint64_t replay_rate_milli,
                                std::uint64_t handoff_retries,
                                std::uint64_t ring_overruns,
                                std::uint64_t recovery_elapsed_ms);

    bool is_open() const { return mapping_ != 0; }
    const ShmRingHeader* header() const { return header_; }
    std::uint64_t generation() const;
    std::uint64_t latest_event_id() const;
    std::uint64_t latest_feed_sequence() const;
    ContinuityState continuity_state() const;
    ReadinessState readiness_state() const;
    std::size_t mapping_bytes() const { return mapping_bytes_; }

private:
    ShmSlotPrefix* slot(std::uint64_t event_id) const;

    int fd_;
    unsigned char* mapping_;
    std::size_t mapping_bytes_;
    ShmRingHeader* header_;
    bool producer_;
};

enum ReplayMode {
    kReplayJournal = 0,
    kReplayHandoff = 1,
    kReplayLive = 2,
    kReplayInvalid = 3,
};

enum ReplayReadStatus {
    kReplayReadEvent = 0,
    kReplayReadWouldBlock = 1,
    kReplayReadInvalid = 2,
    kReplayReadError = 3,
};

enum ReplayOpenStatus {
    kReplayOpenNotAttempted = 0,
    kReplayOpenOk = 1,
    kReplayOpenJournalFailed = 2,
    kReplayOpenShmFailed = 3,
    kReplayOpenMetadataMismatch = 4,
};

class ReplayHandoffConsumer {
public:
    ReplayHandoffConsumer();
    ~ReplayHandoffConsumer();

    bool open(const JournalConfig& journal_config,
              const std::string& shm_path,
              bool publish_legacy_shared_readiness = true);
    void close();
    ReplayReadStatus next(CanonicalEvent* event,
                          void* payload,
                          std::size_t payload_capacity);
    void publish_metrics(std::uint64_t replay_rate_milli,
                         std::uint64_t recovery_elapsed_ms);

    ReplayMode mode() const { return mode_; }
    ReplayOpenStatus last_open_status() const { return last_open_status_; }
    std::uint64_t next_event_id() const { return next_event_id_; }
    std::uint64_t replayed_events() const { return replayed_events_; }
    std::uint64_t live_events() const { return live_events_; }
    std::uint64_t handoff_retries() const { return handoff_retries_; }
    std::uint64_t ring_overruns() const { return ring_overruns_; }
    std::uint64_t generation() const { return generation_; }
    std::uint64_t latest_event_id() const { return ring_.latest_event_id(); }
    std::uint64_t latest_feed_sequence() const {
        return ring_.latest_feed_sequence();
    }
    std::uint64_t replay_lag() const {
        const std::uint64_t replay_event = next_event_id_ > 0U
            ? next_event_id_ - 1U : 0U;
        const std::uint64_t latest = ring_.latest_event_id();
        return latest > replay_event ? latest - replay_event : 0U;
    }

private:
    ReplayReadStatus read_ring(CanonicalEvent* event,
                               void* payload,
                               std::size_t payload_capacity);
    void invalidate();

    JournalReader reader_;
    ShmEventRing ring_;
    ReplayMode mode_;
    ReplayOpenStatus last_open_status_;
    std::uint64_t next_event_id_;
    std::uint64_t generation_;
    std::uint64_t replayed_events_;
    std::uint64_t live_events_;
    std::uint64_t handoff_retries_;
    std::uint64_t ring_overruns_;
    bool publish_legacy_shared_readiness_;
};

std::uint32_t crc32(const void* data, std::size_t bytes);
std::uint64_t monotonic_time_ns();
std::uint64_t realtime_ns();

}  // namespace sze_recovery

#endif  // DEEPWIN_SZE_RECOVERABLE_H
