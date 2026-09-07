#include "sse_event_journal.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>

#include <sys/stat.h>

namespace sse_pipeline {
namespace {

const std::uint32_t kShanghaiSourceId = 89U;
const char kShanghaiJournalPrefix[] = "sse";
const std::uint64_t kDefaultSegmentBytes = 1ULL << 30U;
const std::uint64_t kDefaultMinFreeBytes = 80ULL << 30U;
const std::uint32_t kFirstChannel = 1U;
const std::uint32_t kLastChannel = 6U;
const std::uint16_t kTickPayloadBytes = 72U;
const std::uint16_t kSnapshotPayloadBytes = 440U;

bool is_tick_kind(std::uint8_t kind) {
    return kind == static_cast<std::uint8_t>(kTick) ||
           kind == static_cast<std::uint8_t>(kTickSample);
}

void clear_error(std::string* error) {
    if (error) error->clear();
}

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

EventJournalReader::NextResult read_error(std::string* error,
                                          const std::string& message) {
    if (error) *error = message;
    return EventJournalReader::Error;
}

bool path_exists(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0;
}

std::string first_segment_path(const JournalOptions& options) {
    std::ostringstream path;
    path << options.directory;
    if (!options.directory.empty() &&
        options.directory[options.directory.size() - 1U] != '/') {
        path << '/';
    }
    path << kShanghaiJournalPrefix << '_' << options.trading_day
         << "_s" << kShanghaiSourceId << "_000000.szej";
    return path.str();
}

bool valid_event_options(const JournalOptions& options, std::string* error) {
    if (options.directory.empty()) {
        return fail(error, "Shanghai journal directory is empty");
    }
    if (options.trading_day < 20000101U || options.trading_day > 99991231U) {
        return fail(error, "invalid Shanghai journal trading day");
    }
    if (options.segment_bytes == 0U ||
        options.segment_bytes > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        return fail(error, "invalid Shanghai journal segment size");
    }
    return true;
}

bool valid_symbol(const Event& event) {
    for (std::size_t index = 0U; index < 6U; ++index) {
        if (event.security_id[index] < '0' || event.security_id[index] > '9') {
            return false;
        }
    }
    return (event.security_id[6] == '\0' || event.security_id[6] == ' ') &&
           (event.security_id[7] == '\0' || event.security_id[7] == ' ');
}

bool valid_event(const Event& event, std::uint32_t trading_day,
                 std::uint64_t expected_event_id, std::string* error) {
    if (event.version != static_cast<std::uint16_t>(kEventVersion)) {
        return fail(error, "Shanghai event version is invalid");
    }
    if (event.event_id == 0U || event.event_id != expected_event_id) {
        return fail(error, "Shanghai event id is not contiguous");
    }
    if (event.trading_day != trading_day) {
        return fail(error, "Shanghai event trading day does not match journal");
    }
    if (!is_tick_kind(event.kind) &&
        event.kind != static_cast<std::uint8_t>(kSnapshot)) {
        return fail(error, "Shanghai event kind is invalid");
    }
    if (!valid_symbol(event)) {
        return fail(error, "Shanghai event security id must be six digits");
    }

    if (event.channel_no == 0U) {
        if (event.kind != static_cast<std::uint8_t>(kSnapshot) ||
            event.shard_id != kUnassignedShard) {
            return fail(error, "only an unassigned Shanghai snapshot may omit a channel");
        }
    } else if (event.channel_no < kFirstChannel ||
               event.channel_no > kLastChannel ||
               event.shard_id == kUnassignedShard) {
        return fail(error, "Shanghai channel must be 1..6 with an assigned shard");
    }

    const std::uint16_t expected_payload =
        is_tick_kind(event.kind)
            ? kTickPayloadBytes : kSnapshotPayloadBytes;
    if (event.payload_size != expected_payload) {
        return fail(error, "Shanghai event payload size does not match its kind");
    }
    if (is_tick_kind(event.kind)) {
        const std::uint16_t raw_channel =
            static_cast<std::uint16_t>(event.payload[17]) |
            static_cast<std::uint16_t>(event.payload[18]) << 8U;
        if (raw_channel != event.channel_no) {
            return fail(error, "Shanghai tick payload channel does not match its envelope");
        }
    }
    return true;
}

std::string journal_status_message(const char* operation,
                                   sze_recovery::JournalStatus status) {
    std::ostringstream message;
    message << operation << " failed (journal status "
            << static_cast<int>(status) << ')';
    return message.str();
}

bool valid_canonical_event(const sze_recovery::CanonicalEvent& canonical,
                           const Event& inner, const JournalOptions& options,
                           std::uint64_t expected_event_id,
                           std::string* error) {
    if (canonical.source_id != kShanghaiSourceId) {
        return fail(error, "Shanghai journal source id is invalid");
    }
    if (canonical.trading_day != options.trading_day ||
        inner.trading_day != options.trading_day) {
        return fail(error, "Shanghai journal trading day is invalid");
    }
    if (canonical.event_id != expected_event_id ||
        inner.event_id != expected_event_id ||
        canonical.event_id != inner.event_id) {
        return fail(error, "Shanghai journal event id is not contiguous");
    }
    if (canonical.payload_size != sizeof(Event) ||
        canonical.record_kind != sze_recovery::kRecordMarketData) {
        return fail(error, "Shanghai journal record envelope is invalid");
    }
    if (canonical.channel_number != inner.channel_no ||
        canonical.message_type != inner.kind ||
        canonical.feed_sequence != inner.sequence ||
        canonical.channel_sequence != inner.sequence ||
        canonical.receive_mono_ns != inner.receive_mono_ns ||
        canonical.exchange_time != inner.exchange_time_us ||
        canonical.flags != inner.flags) {
        return fail(error, "Shanghai journal record does not match its embedded event");
    }
    return valid_event(inner, options.trading_day, expected_event_id, error);
}

}  // namespace

JournalOptions::JournalOptions()
    : directory(),
      trading_day(0U),
      segment_bytes(kDefaultSegmentBytes),
      min_free_bytes(kDefaultMinFreeBytes) {
}

EventJournal::EventJournal()
    : writer_(), options_(), next_event_id_(1U), open_(false) {
}

EventJournal::~EventJournal() {
    if (open_) writer_.close(false);
}

sze_recovery::JournalConfig EventJournal::config_for(
    const JournalOptions& options) const {
    sze_recovery::JournalConfig config;
    config.directory = options.directory;
    config.prefix = kShanghaiJournalPrefix;
    config.trading_day = options.trading_day;
    config.source_id = kShanghaiSourceId;
    config.segment_bytes = options.segment_bytes;
    config.max_payload_bytes = static_cast<std::uint32_t>(sizeof(Event));
    config.generation = 0U;
    config.min_free_bytes_after_allocate = options.min_free_bytes;
    return config;
}

bool EventJournal::open(const JournalOptions& options, std::string* error) {
    clear_error(error);
    if (open_) {
        writer_.close(false);
        open_ = false;
    }
    if (!valid_event_options(options, error)) return false;

    // A new writer never resumes a previous epoch.  Check before calling the
    // underlying writer, which otherwise marks an existing segment unclean.
    if (path_exists(first_segment_path(options))) {
        return fail(error, "Shanghai journal already exists for this trading day");
    }

    const sze_recovery::JournalOpenResult result = writer_.open(config_for(options));
    if (result.status != sze_recovery::kJournalOk) {
        writer_.close(false);
        return fail(error, journal_status_message("Shanghai journal open", result.status));
    }
    if (result.existing) {
        writer_.close(false);
        return fail(error, "Shanghai journal already exists for this trading day");
    }
    options_ = options;
    next_event_id_ = 1U;
    open_ = true;
    return true;
}

bool EventJournal::append(const Event& event, std::string* error) {
    clear_error(error);
    if (!open_) return fail(error, "Shanghai journal is not open");
    if (!valid_event(event, options_.trading_day, next_event_id_, error)) return false;

    sze_recovery::CanonicalEvent canonical;
    std::memset(&canonical, 0, sizeof(canonical));
    canonical.event_id = event.event_id;
    canonical.feed_sequence = event.sequence;
    canonical.channel_sequence = event.sequence;
    canonical.receive_mono_ns = event.receive_mono_ns;
    canonical.exchange_time = event.exchange_time_us;
    canonical.trading_day = event.trading_day;
    canonical.source_id = static_cast<std::uint16_t>(kShanghaiSourceId);
    canonical.channel_number = event.channel_no;
    canonical.payload_size = static_cast<std::uint16_t>(sizeof(Event));
    canonical.message_type = event.kind;
    canonical.record_kind = sze_recovery::kRecordMarketData;
    canonical.flags = event.flags;

    const sze_recovery::JournalStatus status = writer_.append(
        &canonical, &event);
    if (status != sze_recovery::kJournalOk) {
        return fail(error, journal_status_message("Shanghai journal append", status));
    }
    ++next_event_id_;
    return true;
}

bool EventJournal::flush(std::string* error) {
    clear_error(error);
    if (!open_) return fail(error, "Shanghai journal is not open");
    const sze_recovery::JournalStatus status = writer_.flush(true);
    if (status != sze_recovery::kJournalOk) {
        return fail(error, journal_status_message("Shanghai journal flush", status));
    }
    return true;
}

bool EventJournal::close(bool clean_shutdown, std::string* error) {
    clear_error(error);
    if (!open_) return true;
    const sze_recovery::JournalStatus status = writer_.close(clean_shutdown);
    open_ = false;
    if (status != sze_recovery::kJournalOk) {
        return fail(error, journal_status_message("Shanghai journal close", status));
    }
    return true;
}

EventJournalReader::EventJournalReader()
    : reader_(), options_(), next_event_id_(1U), open_(false), failed_(false) {
}

EventJournalReader::~EventJournalReader() {
    close();
}

sze_recovery::JournalConfig EventJournalReader::config_for(
    const JournalOptions& options) const {
    sze_recovery::JournalConfig config;
    config.directory = options.directory;
    config.prefix = kShanghaiJournalPrefix;
    config.trading_day = options.trading_day;
    config.source_id = kShanghaiSourceId;
    config.segment_bytes = options.segment_bytes;
    config.max_payload_bytes = static_cast<std::uint32_t>(sizeof(Event));
    config.generation = 0U;
    config.min_free_bytes_after_allocate = options.min_free_bytes;
    return config;
}

bool EventJournalReader::open(const JournalOptions& options, std::string* error) {
    clear_error(error);
    close();
    if (!valid_event_options(options, error)) return false;

    const sze_recovery::JournalOpenResult result = reader_.open(config_for(options));
    if (result.status != sze_recovery::kJournalOk || !result.existing) {
        return fail(error, journal_status_message("Shanghai journal reader open",
                                                  result.status));
    }
    if (result.unclean_restart || result.corrupt_tail ||
        result.continuity_state == sze_recovery::kContinuityInvalid) {
        reader_.close();
        return fail(error, "Shanghai journal is unclean or has invalid continuity state");
    }
    if (reader_.next_event_id() != 1U) {
        reader_.close();
        return fail(error, "Shanghai journal does not start at event id 1");
    }
    options_ = options;
    next_event_id_ = 1U;
    open_ = true;
    failed_ = false;
    return true;
}

EventJournalReader::NextResult EventJournalReader::next(Event* event,
                                                         std::string* error) {
    clear_error(error);
    if (!open_ || failed_) {
        return read_error(error, failed_
            ? "Shanghai journal reader is invalid"
            : "Shanghai journal reader is not open");
    }
    if (!event) {
        failed_ = true;
        return read_error(error, "Shanghai journal reader received a null event");
    }

    sze_recovery::CanonicalEvent canonical;
    Event inner;
    std::memset(&canonical, 0, sizeof(canonical));
    std::memset(&inner, 0, sizeof(inner));
    const sze_recovery::JournalStatus status = reader_.next(
        &canonical, &inner, sizeof(inner));
    if (status == sze_recovery::kJournalEnd) return End;
    if (status != sze_recovery::kJournalOk) {
        failed_ = true;
        return read_error(error, journal_status_message(
            "Shanghai journal read", status));
    }
    if (!valid_canonical_event(canonical, inner, options_, next_event_id_, error)) {
        failed_ = true;
        return Error;
    }
    *event = inner;
    ++next_event_id_;
    return Record;
}

bool EventJournalReader::close() {
    const sze_recovery::JournalStatus status = reader_.close();
    open_ = false;
    failed_ = false;
    next_event_id_ = 1U;
    return status == sze_recovery::kJournalOk;
}

}  // namespace sse_pipeline
