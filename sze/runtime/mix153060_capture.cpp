#include "sze/runtime/mix153060_capture.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mix153060 {

namespace {

std::string normalize_instrument(const std::string& value) {
    const std::string::size_type dot = value.find('.');
    return dot == std::string::npos ? value : value.substr(0, dot);
}

bool path_is_directory(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool ensure_directory(const std::string& path, std::string* error) {
    if (path.empty()) {
        if (error != 0) {
            *error = "capture directory is empty";
        }
        return false;
    }
    std::string current;
    if (path[0] == '/') {
        current = "/";
    }
    std::size_t begin = path[0] == '/' ? 1 : 0;
    while (begin <= path.size()) {
        const std::size_t slash = path.find('/', begin);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        if (end > begin) {
            if (!current.empty() && current[current.size() - 1] != '/') {
                current += '/';
            }
            current += path.substr(begin, end - begin);
            if (!path_is_directory(current) && ::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                if (error != 0) {
                    std::ostringstream message;
                    message << "cannot create capture directory " << current << ": "
                            << std::strerror(errno);
                    *error = message.str();
                }
                return false;
            }
            if (!path_is_directory(current)) {
                if (error != 0) {
                    *error = "capture path exists but is not a directory: " + current;
                }
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        begin = slash + 1;
    }
    return path_is_directory(path);
}

bool file_is_empty(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) != 0 || info.st_size == 0;
}

std::uint64_t monotonic_time_ns() {
    struct timespec value;
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0U;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

bool header_contains(const std::string& path, const char* field) {
    std::ifstream input(path.c_str());
    std::string header;
    return input.is_open() && std::getline(input, header) &&
           header.find(field) != std::string::npos;
}

const char* order_kind_name(OrderKind kind) {
    switch (kind) {
    case OrderKind::kMarket:
        return "market";
    case OrderKind::kSelfBest:
        return "self-best";
    case OrderKind::kLimit:
    default:
        return "limit";
    }
}

const char* trade_kind_name(TradeKind kind) {
    return kind == TradeKind::kCancel ? "cancel" : "fill";
}

void append_book_columns(std::ostringstream* row, const Sample& sample) {
    for (std::size_t level = 0; level < 10; ++level) {
        *row << ',' << sample.bid_price[level];
    }
    for (std::size_t level = 0; level < 10; ++level) {
        *row << ',' << sample.ask_price[level];
    }
    for (std::size_t level = 0; level < 10; ++level) {
        *row << ',' << sample.bid_volume[level];
    }
    for (std::size_t level = 0; level < 10; ++level) {
        *row << ',' << sample.ask_volume[level];
    }
}

void append_book_header(std::ostringstream* header) {
    for (std::size_t level = 0; level < 10; ++level) {
        *header << ",bid_price_" << (level + 1);
    }
    for (std::size_t level = 0; level < 10; ++level) {
        *header << ",ask_price_" << (level + 1);
    }
    for (std::size_t level = 0; level < 10; ++level) {
        *header << ",bid_volume_" << (level + 1);
    }
    for (std::size_t level = 0; level < 10; ++level) {
        *header << ",ask_volume_" << (level + 1);
    }
}

std::uint64_t realtime_time_ns() {
    struct timespec value;
    if (::clock_gettime(CLOCK_REALTIME, &value) != 0) {
        return 0U;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

std::uint32_t crc32_bytes(const void* data, std::size_t size) {
    static std::uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (std::uint32_t i = 0; i < 256U; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1U) ^ (0xedb88320U &
                    static_cast<std::uint32_t>(-(static_cast<int>(value & 1U))));
            }
            table[i] = value;
        }
        initialized = true;
    }
    std::uint32_t value = 0xffffffffU;
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        value = table[(value ^ bytes[i]) & 0xffU] ^ (value >> 8U);
    }
    return value ^ 0xffffffffU;
}

void copy_instrument(char output[9], const std::string& instrument) {
    std::memset(output, 0, 9U);
    const std::string normalized = normalize_instrument(instrument);
    std::memcpy(output, normalized.data(), std::min<std::size_t>(8U, normalized.size()));
}

enum PredictionLogRecordType {
    kLogCompactSample = 1,
    kLogFullSample = 2,
    kLogOrder = 3,
    kLogTrade = 4,
    kLogMarketResolution = 5,
};

#pragma pack(push, 1)
struct PredictionLogFileHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t endian_marker;
    std::uint32_t feature_count;
    std::uint64_t created_unix_ns;
    std::uint8_t reserved[32];
};

struct PredictionLogRecordHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint8_t record_type;
    std::uint8_t detail_level;
    std::uint32_t total_bytes;
    std::uint32_t payload_bytes;
    std::uint64_t record_sequence;
    std::uint32_t payload_crc32;
    std::uint32_t header_crc32;
};

struct CompactSamplePayload {
    char instrument[9];
    std::uint8_t trigger_flags;
    std::uint8_t source_continuity_valid;
    std::int16_t source;
    std::int64_t exchange_time_us;
    std::int64_t local_time_us;
    std::int64_t app_sequence;
    std::int64_t cut_index;
    std::int64_t row_in_stock_day;
    std::int64_t window_start_exchange_time_us;
    std::int64_t window_start_app_sequence;
    std::int64_t window_start_cut_index;
    std::int64_t framework_receive_time;
    double last_price;
    double mid_price;
    double turnover;
    double volume;
    float raw_prediction;
    std::uint64_t model_latency_ns;
};

struct FullSamplePayload {
    CompactSamplePayload compact;
    double bid_price[10];
    double ask_price[10];
    double bid_volume[10];
    double ask_volume[10];
    float factors[kFeatureCount];
};

struct EventPayload {
    char instrument[9];
    std::uint8_t event_type;
    std::uint8_t event_kind;
    std::uint8_t accepted;
    std::uint8_t source_continuity_valid;
    std::int16_t source;
    std::uint16_t samples_emitted;
    std::int64_t app_sequence;
    std::int64_t exchange_time_us;
    std::int64_t local_time_us;
    std::int64_t framework_receive_time;
    double price;
    std::int64_t volume;
    std::int64_t buy_order_id;
    std::int64_t sell_order_id;
    std::uint64_t book_mutation_ns;
    std::uint64_t sample_work_ns;
    std::uint64_t total_runtime_ns;
};

struct MarketResolutionPayload {
    char instrument[9];
    std::uint8_t from_linked_fill;
    std::uint8_t buy;
    std::uint8_t source_continuity_valid;
    std::int16_t source;
    std::int64_t app_sequence;
    std::int64_t exchange_time_us;
    std::int64_t local_time_us;
    std::int64_t framework_receive_time;
    double price;
    std::int64_t volume;
};
#pragma pack(pop)

static_assert(sizeof(PredictionLogFileHeader) == 64U, "unexpected prediction log header");
static_assert(sizeof(PredictionLogRecordHeader) == 32U, "unexpected prediction record header");

void fill_compact_sample(CompactSamplePayload* output,
                         const std::string& instrument,
                         const Sample& sample,
                         short source,
                         std::int64_t framework_receive_time,
                         float raw_prediction,
                         std::uint64_t model_latency_ns,
                         bool source_continuity_valid) {
    std::memset(output, 0, sizeof(*output));
    copy_instrument(output->instrument, instrument);
    output->trigger_flags = static_cast<std::uint8_t>(
        (sample.amount_trigger ? 1U : 0U) |
        (sample.time_trigger ? 2U : 0U) |
        (sample.change_trigger ? 4U : 0U));
    output->source_continuity_valid = source_continuity_valid ? 1U : 0U;
    output->source = source;
    output->exchange_time_us = sample.exchange_time_us;
    output->local_time_us = sample.local_time_us;
    output->app_sequence = sample.app_sequence;
    output->cut_index = sample.cut_index;
    output->row_in_stock_day = sample.row_in_stock_day;
    output->window_start_exchange_time_us = sample.window_start_exchange_time_us;
    output->window_start_app_sequence = sample.window_start_app_sequence;
    output->window_start_cut_index = sample.window_start_cut_index;
    output->framework_receive_time = framework_receive_time;
    output->last_price = sample.last_price;
    output->mid_price = sample.mid_price;
    output->turnover = sample.turnover;
    output->volume = sample.volume;
    output->raw_prediction = raw_prediction;
    output->model_latency_ns = model_latency_ns;
}

}  // namespace

class AsyncPredictionLog {
public:
    AsyncPredictionLog(const std::string& path,
                       std::size_t batch_bytes,
                       std::size_t queue_bytes)
        : path_(path), fd_(-1), batch_bytes_(std::max<std::size_t>(4096U, batch_bytes)),
          queue_limit_bytes_(std::max<std::size_t>(batch_bytes_ * 2U, queue_bytes)),
          queued_bytes_(0U), sequence_(0U), stopping_(false), writing_(false),
          failed_(false) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (fd_ < 0) {
            error_ = std::string("cannot open prediction log: ") + std::strerror(errno);
            failed_ = true;
            return;
        }
        struct stat info;
        if (::fstat(fd_, &info) != 0) {
            error_ = std::string("cannot stat prediction log: ") + std::strerror(errno);
            failed_ = true;
            ::close(fd_);
            fd_ = -1;
            return;
        }
        if (info.st_size == 0) {
            PredictionLogFileHeader header;
            std::memset(&header, 0, sizeof(header));
            std::memcpy(header.magic, "SZEPLG1", 7U);
            header.version = 1U;
            header.header_bytes = sizeof(header);
            header.endian_marker = 0x01020304U;
            header.feature_count = static_cast<std::uint32_t>(kFeatureCount);
            header.created_unix_ns = realtime_time_ns();
            if (!write_all(&header, sizeof(header))) {
                failed_ = true;
                return;
            }
        } else {
            PredictionLogFileHeader header;
            if (::pread(fd_, &header, sizeof(header), 0) != static_cast<ssize_t>(sizeof(header)) ||
                std::memcmp(header.magic, "SZEPLG1", 7U) != 0 ||
                header.version != 1U || header.header_bytes != sizeof(header) ||
                header.feature_count != kFeatureCount) {
                error_ = "prediction log header mismatch";
                failed_ = true;
                return;
            }
        }
        worker_ = std::thread(&AsyncPredictionLog::run, this);
    }

    ~AsyncPredictionLog() {
        flush();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (fd_ >= 0) {
            ::fdatasync(fd_);
            ::close(fd_);
        }
    }

    bool ready() const { return !failed_ && fd_ >= 0; }
    const std::string& error() const { return error_; }
    const std::string& path() const { return path_; }

    bool append(std::uint8_t record_type, std::uint8_t detail_level,
                const void* payload, std::size_t payload_bytes) {
        if (!ready() || payload == 0 || payload_bytes == 0U ||
            payload_bytes > 16U * 1024U * 1024U) {
            return false;
        }
        const std::uint32_t payload_crc = crc32_bytes(payload, payload_bytes);
        std::lock_guard<std::mutex> lock(mutex_);
        PredictionLogRecordHeader header;
        std::memset(&header, 0, sizeof(header));
        header.magic = 0x474c5a53U;
        header.version = 1U;
        header.record_type = record_type;
        header.detail_level = detail_level;
        header.total_bytes = static_cast<std::uint32_t>(sizeof(header) + payload_bytes);
        header.payload_bytes = static_cast<std::uint32_t>(payload_bytes);
        header.record_sequence = ++sequence_;
        header.payload_crc32 = payload_crc;
        header.header_crc32 = 0U;
        header.header_crc32 = crc32_bytes(&header, sizeof(header));

        if (failed_ || queued_bytes_ + active_.size() + header.total_bytes > queue_limit_bytes_) {
            if (!failed_) {
                error_ = "prediction log async queue exhausted";
                failed_ = true;
            }
            return false;
        }
        active_.append(reinterpret_cast<const char*>(&header), sizeof(header));
        active_.append(reinterpret_cast<const char*>(payload), payload_bytes);
        if (active_.size() >= batch_bytes_) {
            publish_locked();
        }
        return true;
    }

    void publish() {
        std::lock_guard<std::mutex> lock(mutex_);
        publish_locked();
    }

    void flush() {
        if (fd_ < 0) {
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        publish_locked();
        drained_.wait(lock, [this] { return queue_.empty() && !writing_; });
        lock.unlock();
        if (::fdatasync(fd_) != 0 && !failed_) {
            error_ = std::string("prediction log fdatasync failed: ") + std::strerror(errno);
            failed_ = true;
        }
    }

private:
    bool write_all(const void* data, std::size_t size) {
        const char* cursor = static_cast<const char*>(data);
        while (size > 0U) {
            const ssize_t written = ::write(fd_, cursor, size);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                error_ = std::string("prediction log write failed: ") + std::strerror(errno);
                return false;
            }
            cursor += written;
            size -= static_cast<std::size_t>(written);
        }
        return true;
    }

    void publish_locked() {
        if (active_.empty()) {
            return;
        }
        queued_bytes_ += active_.size();
        queue_.push_back(std::string());
        queue_.back().swap(active_);
        cv_.notify_one();
    }

    void run() {
        for (;;) {
            std::string batch;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty() && stopping_) {
                    break;
                }
                batch.swap(queue_.front());
                queue_.pop_front();
                queued_bytes_ -= batch.size();
                writing_ = true;
            }
            if (!write_all(batch.data(), batch.size())) {
                std::lock_guard<std::mutex> lock(mutex_);
                failed_ = true;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                writing_ = false;
                if (queue_.empty()) {
                    drained_.notify_all();
                }
            }
        }
    }

    std::string path_;
    int fd_;
    std::size_t batch_bytes_;
    std::size_t queue_limit_bytes_;
    std::size_t queued_bytes_;
    std::uint64_t sequence_;
    bool stopping_;
    bool writing_;
    std::atomic<bool> failed_;
    std::string error_;
    std::string active_;
    std::deque<std::string> queue_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable drained_;
};

CaptureConfig::CaptureConfig()
    : enabled(false),
      directory(),
      prefix("mix153060"),
      instruments(),
      output_format("csv"),
      detail_instruments(),
      record_events(true),
      record_samples(true),
      capture_only(false),
      flush_rows(4096),
      flush_interval_ms(1000),
      log_batch_bytes(1U << 20U),
      log_queue_bytes(256U << 20U) {}

Capture::Capture(const CaptureConfig& config)
    : enabled_(config.enabled),
      ready_(!config.enabled),
      record_events_(config.record_events),
      record_samples_(config.record_samples),
      flush_rows_(config.flush_rows == 0 ? 4096 : config.flush_rows),
      flush_interval_ms_(config.flush_interval_ms == 0 ? 1000 : config.flush_interval_ms),
      pending_rows_(0),
      last_flush_mono_ns_(monotonic_time_ns()),
      instruments_(),
      detail_instruments_(),
      binary_log_(config.output_format == "sze_log" || config.output_format == "binary"),
      error_(),
      events_path_(),
      samples_path_(),
      market_resolutions_path_(),
      events_buffer_(),
      samples_buffer_(),
      market_resolutions_buffer_(),
      events_file_(),
      samples_file_(),
      market_resolutions_file_(),
      prediction_log_() {
    if (!enabled_) {
        return;
    }
    if (!record_events_ && !record_samples_) {
        fail("capture enabled but both events and samples are disabled");
        return;
    }
    if (config.prefix.empty() || config.prefix.find('/') != std::string::npos ||
        config.prefix.find('\\') != std::string::npos) {
        fail("capture prefix must be a non-empty file name");
        return;
    }
    for (std::size_t i = 0; i < config.instruments.size(); ++i) {
        const std::string instrument = normalize_instrument(config.instruments[i]);
        if (!instrument.empty()) {
            instruments_.insert(instrument);
        }
    }
    for (std::size_t i = 0; i < config.detail_instruments.size(); ++i) {
        const std::string instrument = normalize_instrument(config.detail_instruments[i]);
        if (!instrument.empty()) {
            detail_instruments_.insert(instrument);
        }
    }
    if (instruments_.empty()) {
        fail("capture requires a non-empty instrument allowlist");
        return;
    }
    if (!ensure_directory(config.directory, &error_)) {
        ready_ = false;
        return;
    }
    if (binary_log_) {
        events_path_ = config.directory + "/" + config.prefix + "_predictions.szelog";
        samples_path_ = events_path_;
        market_resolutions_path_ = events_path_;
        prediction_log_.reset(new AsyncPredictionLog(
            events_path_, config.log_batch_bytes, config.log_queue_bytes));
        if (!prediction_log_->ready()) {
            fail(prediction_log_->error());
            return;
        }
        ready_ = true;
        return;
    }
    events_path_ = config.directory + "/" + config.prefix + "_events.csv";
    samples_path_ = config.directory + "/" + config.prefix + "_samples.csv";
    market_resolutions_path_ = config.directory + "/" + config.prefix +
                               "_market_resolutions.csv";
    if (record_events_) {
        const bool empty = file_is_empty(events_path_);
        if (!empty && !header_contains(events_path_, "source_continuity_valid")) {
            fail("event capture file uses an older header; rotate the output file: " +
                 events_path_);
            return;
        }
        events_file_.open(events_path_.c_str(), std::ios::out | std::ios::app);
        if (!events_file_.is_open()) {
            fail("cannot open event capture file: " + events_path_);
            return;
        }
        if (empty) {
            events_file_ << "instrument,event_type,event_kind,source,framework_receive_time,"
                            "exchange_time_us,local_time_us,app_sequence,price,volume,accepted,"
                            "book_mutation_ns,sample_work_ns,total_runtime_ns,samples_emitted,"
                            "buy_order_id,sell_order_id,source_continuity_valid\n";
            events_file_.flush();
        }
    }
    if (record_samples_) {
        const bool empty = file_is_empty(samples_path_);
        if (!empty && !header_contains(samples_path_, "source_continuity_valid")) {
            fail("sample capture file uses an older header; rotate the output file: " +
                 samples_path_);
            return;
        }
        samples_file_.open(samples_path_.c_str(), std::ios::out | std::ios::app);
        if (!samples_file_.is_open()) {
            fail("cannot open sample capture file: " + samples_path_);
            return;
        }
        if (empty) {
            samples_file_ << "instrument,exchange_time_us,local_time_us,app_sequence,cut_index,"
                             "row_in_stock_day,window_start_exchange_time_us,window_start_app_sequence,"
                             "window_start_cut_index,last_price,mid_price,turnover,volume,"
                             "amount_trigger,time_trigger,change_trigger,source,"
                             "framework_receive_time,raw_prediction,model_latency_ns";
            std::ostringstream header;
            append_book_header(&header);
            samples_file_ << header.str();
            for (std::size_t i = 0; i < kFeatureCount; ++i) {
                samples_file_ << ',' << factor_names()[i];
            }
            samples_file_ << ",source_continuity_valid\n";
            samples_file_.flush();
        }
    }
    {
        const bool empty = file_is_empty(market_resolutions_path_);
        if (!empty && !header_contains(market_resolutions_path_, "resolution_source")) {
            fail("market resolution capture file uses an older header; rotate the output file: " +
                 market_resolutions_path_);
            return;
        }
        market_resolutions_file_.open(market_resolutions_path_.c_str(),
                                      std::ios::out | std::ios::app);
        if (!market_resolutions_file_.is_open()) {
            fail("cannot open market resolution capture file: " + market_resolutions_path_);
            return;
        }
        if (empty) {
            market_resolutions_file_ << "instrument,app_sequence,exchange_time_us,local_time_us,"
                                        "derived_price,volume,buy,resolution_source,source,"
                                        "framework_receive_time,source_continuity_valid\n";
            market_resolutions_file_.flush();
        }
    }
    ready_ = true;
}

Capture::~Capture() {
    flush();
}

bool Capture::ready() const {
    return ready_;
}

bool Capture::enabled_for(const std::string& instrument) const {
    return enabled_ && ready_ && instruments_.find(normalize_instrument(instrument)) != instruments_.end();
}

bool Capture::detail_enabled_for(const std::string& instrument) const {
    return detail_instruments_.find(normalize_instrument(instrument)) !=
           detail_instruments_.end();
}

const std::string& Capture::error() const {
    return error_;
}

const std::string& Capture::events_path() const {
    return events_path_;
}

const std::string& Capture::samples_path() const {
    return samples_path_;
}

const std::string& Capture::market_resolutions_path() const {
    return market_resolutions_path_;
}

void Capture::fail(const std::string& message) {
    ready_ = false;
    error_ = message;
    if (events_file_.is_open()) {
        events_file_.close();
    }
    if (samples_file_.is_open()) {
        samples_file_.close();
    }
    if (market_resolutions_file_.is_open()) {
        market_resolutions_file_.close();
    }
}

void Capture::record_event(const std::string& instrument,
                           const char* event_type,
                           const char* event_kind,
                           std::int64_t app_sequence,
                           std::int64_t exchange_time_us,
                           std::int64_t local_time_us,
                           double price,
                           std::int64_t volume,
                           std::int64_t buy_order_id,
                           std::int64_t sell_order_id,
                           short source,
                           std::int64_t framework_receive_time,
                           const EventTiming& timing,
                           bool accepted,
                           std::size_t samples_emitted,
                           bool source_continuity_valid) {
    if (!enabled_for(instrument) || !record_events_) {
        return;
    }
    if (binary_log_) {
        if (!detail_enabled_for(instrument)) {
            return;
        }
        EventPayload payload;
        std::memset(&payload, 0, sizeof(payload));
        copy_instrument(payload.instrument, instrument);
        payload.event_type = std::strcmp(event_type, "trade") == 0 ? 2U : 1U;
        payload.event_kind = std::strcmp(event_kind, "cancel") == 0 ? 2U :
                             (std::strcmp(event_kind, "market") == 0 ? 1U : 0U);
        payload.accepted = accepted ? 1U : 0U;
        payload.source_continuity_valid = source_continuity_valid ? 1U : 0U;
        payload.source = source;
        payload.samples_emitted = static_cast<std::uint16_t>(samples_emitted);
        payload.app_sequence = app_sequence;
        payload.exchange_time_us = exchange_time_us;
        payload.local_time_us = local_time_us;
        payload.framework_receive_time = framework_receive_time;
        payload.price = price;
        payload.volume = volume;
        payload.buy_order_id = buy_order_id;
        payload.sell_order_id = sell_order_id;
        payload.book_mutation_ns = timing.book_mutation_ns;
        payload.sample_work_ns = timing.sample_work_ns;
        payload.total_runtime_ns = timing.total_runtime_ns;
        if (!prediction_log_->append(kLogOrder + (payload.event_type - 1U), 1U,
                                     &payload, sizeof(payload))) {
            fail(prediction_log_->error());
        }
        return;
    }
    char row[1024];
    const int length = std::snprintf(
        row, sizeof(row),
        "%s,%s,%s,%d,%lld,%lld,%lld,%lld,%.17g,%lld,%d,%llu,%llu,%llu,%llu,%lld,%lld,%d\n",
        normalize_instrument(instrument).c_str(), event_type, event_kind,
        static_cast<int>(source), static_cast<long long>(framework_receive_time),
        static_cast<long long>(exchange_time_us), static_cast<long long>(local_time_us),
        static_cast<long long>(app_sequence), price, static_cast<long long>(volume),
        accepted ? 1 : 0, static_cast<unsigned long long>(timing.book_mutation_ns),
        static_cast<unsigned long long>(timing.sample_work_ns),
        static_cast<unsigned long long>(timing.total_runtime_ns),
        static_cast<unsigned long long>(samples_emitted),
        static_cast<long long>(buy_order_id), static_cast<long long>(sell_order_id),
        source_continuity_valid ? 1 : 0);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(row)) {
        fail("event capture row formatting failed");
        return;
    }
    events_buffer_.append(row, static_cast<std::size_t>(length));
    ++pending_rows_;
    maybe_flush();
}

void Capture::record_order(const std::string& instrument,
                           const OrderEvent& event,
                           short source,
                           std::int64_t framework_receive_time,
                           const EventTiming& timing,
                           bool accepted,
                           std::size_t samples_emitted,
                           bool source_continuity_valid) {
    record_event(instrument, "order", order_kind_name(event.kind), event.app_sequence,
                 event.exchange_time_us, event.local_time_us, event.price, event.volume,
                 event.app_sequence, 0, source, framework_receive_time, timing, accepted,
                 samples_emitted, source_continuity_valid);
}

void Capture::record_trade(const std::string& instrument,
                           const TradeEvent& event,
                           short source,
                           std::int64_t framework_receive_time,
                           const EventTiming& timing,
                           bool accepted,
                           std::size_t samples_emitted,
                           bool source_continuity_valid) {
    record_event(instrument, "trade", trade_kind_name(event.kind), event.app_sequence,
                 event.exchange_time_us, event.local_time_us, event.price, event.volume,
                 event.buy_order_id, event.sell_order_id, source, framework_receive_time,
                 timing, accepted, samples_emitted, source_continuity_valid);
}

void Capture::record_market_resolution(const std::string& instrument,
                                       const OrderEvent& event,
                                       bool from_linked_fill,
                                       short source,
                                       std::int64_t framework_receive_time,
                                       bool source_continuity_valid) {
    if (!enabled_for(instrument)) {
        return;
    }
    if (binary_log_) {
        if (!detail_enabled_for(instrument)) {
            return;
        }
        MarketResolutionPayload payload;
        std::memset(&payload, 0, sizeof(payload));
        copy_instrument(payload.instrument, instrument);
        payload.from_linked_fill = from_linked_fill ? 1U : 0U;
        payload.buy = event.buy ? 1U : 0U;
        payload.source_continuity_valid = source_continuity_valid ? 1U : 0U;
        payload.source = source;
        payload.app_sequence = event.app_sequence;
        payload.exchange_time_us = event.exchange_time_us;
        payload.local_time_us = event.local_time_us;
        payload.framework_receive_time = framework_receive_time;
        payload.price = event.price;
        payload.volume = event.volume;
        if (!prediction_log_->append(kLogMarketResolution, 1U,
                                     &payload, sizeof(payload))) {
            fail(prediction_log_->error());
        }
        return;
    }
    if (!market_resolutions_file_.is_open()) {
        return;
    }
    char row[512];
    const int length = std::snprintf(
        row, sizeof(row), "%s,%lld,%lld,%lld,%.17g,%lld,%d,%s,%d,%lld,%d\n",
        normalize_instrument(instrument).c_str(),
        static_cast<long long>(event.app_sequence),
        static_cast<long long>(event.exchange_time_us),
        static_cast<long long>(event.local_time_us), event.price,
        static_cast<long long>(event.volume), event.buy ? 1 : 0,
        from_linked_fill ? "final-linked-fill" : "opposite-best", static_cast<int>(source),
        static_cast<long long>(framework_receive_time), source_continuity_valid ? 1 : 0);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(row)) {
        fail("market resolution capture row formatting failed");
        return;
    }
    market_resolutions_buffer_.append(row, static_cast<std::size_t>(length));
    ++pending_rows_;
    maybe_flush();
}

void Capture::record_sample(const std::string& instrument,
                            const Sample& sample,
                            short source,
                            std::int64_t framework_receive_time,
                            float raw_prediction,
                            std::uint64_t model_latency_ns,
                            bool source_continuity_valid) {
    if (!enabled_for(instrument) || !record_samples_) {
        return;
    }
    if (binary_log_) {
        CompactSamplePayload compact;
        fill_compact_sample(&compact, instrument, sample, source,
                            framework_receive_time, raw_prediction,
                            model_latency_ns, source_continuity_valid);
        if (detail_enabled_for(instrument)) {
            FullSamplePayload full;
            std::memset(&full, 0, sizeof(full));
            full.compact = compact;
            for (std::size_t level = 0; level < 10U; ++level) {
                full.bid_price[level] = sample.bid_price[level];
                full.ask_price[level] = sample.ask_price[level];
                full.bid_volume[level] = sample.bid_volume[level];
                full.ask_volume[level] = sample.ask_volume[level];
            }
            for (std::size_t index = 0; index < kFeatureCount; ++index) {
                full.factors[index] = sample.factors[index];
            }
            if (!prediction_log_->append(kLogFullSample, 1U, &full, sizeof(full))) {
                fail(prediction_log_->error());
            }
        } else if (!prediction_log_->append(kLogCompactSample, 0U,
                                             &compact, sizeof(compact))) {
            fail(prediction_log_->error());
        }
        ++pending_rows_;
        maybe_flush();
        return;
    }
    std::ostringstream row;
    row << std::setprecision(17);
    row << normalize_instrument(instrument) << ',' << sample.exchange_time_us << ','
        << sample.local_time_us << ',' << sample.app_sequence << ',' << sample.cut_index
        << ',' << sample.row_in_stock_day << ',' << sample.window_start_exchange_time_us
        << ',' << sample.window_start_app_sequence << ',' << sample.window_start_cut_index
        << ',' << sample.last_price << ',' << sample.mid_price << ',' << sample.turnover
        << ',' << sample.volume << ',' << (sample.amount_trigger ? 1 : 0) << ','
        << (sample.time_trigger ? 1 : 0) << ',' << (sample.change_trigger ? 1 : 0) << ','
        << source << ',' << framework_receive_time << ',' << raw_prediction << ','
        << model_latency_ns;
    append_book_columns(&row, sample);
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        row << ',' << sample.factors[i];
    }
    row << ',' << (source_continuity_valid ? 1 : 0) << '\n';
    samples_buffer_ += row.str();
    ++pending_rows_;
    maybe_flush();
}

void Capture::maybe_flush() {
    const std::uint64_t now = monotonic_time_ns();
    const bool timed = now > last_flush_mono_ns_ &&
        now - last_flush_mono_ns_ >=
            static_cast<std::uint64_t>(flush_interval_ms_) * 1000000ULL;
    if (pending_rows_ >= flush_rows_ || timed) {
        if (binary_log_) {
            prediction_log_->publish();
            pending_rows_ = 0U;
            last_flush_mono_ns_ = monotonic_time_ns();
        } else {
            flush();
        }
    }
}

void Capture::flush() {
    if (binary_log_) {
        if (prediction_log_) {
            prediction_log_->flush();
            if (!prediction_log_->ready()) {
                error_ = prediction_log_->error();
            }
        }
        pending_rows_ = 0U;
        last_flush_mono_ns_ = monotonic_time_ns();
        return;
    }
    if (events_file_.is_open() && !events_buffer_.empty()) {
        events_file_.write(events_buffer_.data(), events_buffer_.size());
        events_file_.flush();
        events_buffer_.clear();
    }
    if (samples_file_.is_open() && !samples_buffer_.empty()) {
        samples_file_.write(samples_buffer_.data(), samples_buffer_.size());
        samples_file_.flush();
        samples_buffer_.clear();
    }
    if (market_resolutions_file_.is_open() && !market_resolutions_buffer_.empty()) {
        market_resolutions_file_.write(market_resolutions_buffer_.data(),
                                       market_resolutions_buffer_.size());
        market_resolutions_file_.flush();
        market_resolutions_buffer_.clear();
    }
    pending_rows_ = 0;
    last_flush_mono_ns_ = monotonic_time_ns();
}

}  // namespace mix153060
