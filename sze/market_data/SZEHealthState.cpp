#include "sze/market_data/SZEHealthState.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace sze_health {
namespace {

const char kCaptureMagic[8] = {'S','Z','E','H','C','A','P','1'};
const char kShardMagic[8] = {'S','Z','E','H','S','H','D','1'};
const char kTdMagic[8] = {'S','Z','E','H','T','D','0','1'};

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

bool pid_alive(std::uint32_t value)
{
    const pid_t pid = static_cast<pid_t>(value);
    return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

bool prepare_path(const std::string& path, bool replace_stale)
{
    struct stat info;
    if (::stat(path.c_str(), &info) != 0) {
        return errno == ENOENT;
    }
    if (!replace_stale) {
        return false;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        unsigned char page[kHealthPageBytes];
        const ssize_t count = ::pread(fd, page, sizeof(page), 0);
        ::close(fd);
        if (count == static_cast<ssize_t>(sizeof(page))) {
            std::uint32_t writer_pid = 0U;
            if (std::memcmp(page, kCaptureMagic, 8) == 0) {
                writer_pid = reinterpret_cast<CaptureHealthPage*>(page)->writer_pid;
            } else if (std::memcmp(page, kShardMagic, 8) == 0) {
                writer_pid = reinterpret_cast<RecoveryShardHealthPage*>(page)->writer_pid;
            } else if (std::memcmp(page, kTdMagic, 8) == 0) {
                writer_pid = reinterpret_cast<TdHealthPage*>(page)->writer_pid;
            }
            if (pid_alive(writer_pid)) {
                return false;
            }
        }
    }
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

template <typename T>
bool create_page(const std::string& path, bool replace_stale,
                 const char magic[8], int* fd, T** page)
{
    if (!prepare_path(path, replace_stale)) {
        return false;
    }
    *fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    if (*fd < 0 || ::ftruncate(*fd, kHealthPageBytes) != 0) {
        if (*fd >= 0) ::close(*fd);
        *fd = -1;
        return false;
    }
    void* memory = ::mmap(0, kHealthPageBytes, PROT_READ | PROT_WRITE,
                          MAP_SHARED, *fd, 0);
    if (memory == MAP_FAILED) {
        ::close(*fd);
        *fd = -1;
        return false;
    }
    *page = static_cast<T*>(memory);
    std::memset(*page, 0, kHealthPageBytes);
    std::memcpy((*page)->magic, magic, 8);
    (*page)->version = kHealthFormatVersion;
    (*page)->header_bytes = kHealthPageBytes;
    (*page)->writer_pid = static_cast<std::uint32_t>(::getpid());
    return true;
}

}  // namespace

CaptureHealthWriter::CaptureHealthWriter() : fd_(-1), page_(0) {}
CaptureHealthWriter::~CaptureHealthWriter() { close(); }

bool CaptureHealthWriter::create(const std::string& path,
                                 std::uint32_t trading_day,
                                 std::uint32_t source_id,
                                 std::uint64_t generation,
                                 bool replace_stale)
{
    close();
    if (!create_page(path, replace_stale, kCaptureMagic, &fd_, &page_)) {
        return false;
    }
    page_->trading_day = trading_day;
    page_->source_id = source_id;
    page_->generation = generation;
    page_->feed_state = kHealthUnknown;
    page_->journal_state = kHealthUnknown;
    page_->ring_state = kHealthUnknown;
    heartbeat();
    return true;
}

void CaptureHealthWriter::close()
{
    if (page_) ::munmap(page_, kHealthPageBytes);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    page_ = 0;
}

void CaptureHealthWriter::publish_feed(
    HealthState state, sze_recovery::InvalidReason reason, FailureScope scope,
    std::uint32_t channel, std::uint64_t event_id, std::uint64_t feed_sequence)
{
    if (!page_) return;
    store_release(&page_->latest_global_event_id, event_id);
    store_release(&page_->latest_feed_sequence, feed_sequence);
    store_release(&page_->feed_invalid_reason, static_cast<std::uint32_t>(reason));
    store_release(&page_->feed_failure_scope, static_cast<std::uint32_t>(scope));
    store_release(&page_->feed_channel, channel);
    store_release(&page_->feed_state, static_cast<std::uint32_t>(state));
}

void CaptureHealthWriter::publish_journal(HealthState state,
                                          std::uint32_t error,
                                          std::uint64_t journal_errors)
{
    if (!page_) return;
    store_release(&page_->journal_errors, journal_errors);
    store_release(&page_->journal_error, error);
    store_release(&page_->journal_state, static_cast<std::uint32_t>(state));
}

void CaptureHealthWriter::publish_ring(HealthState state, std::uint32_t reason,
                                       std::uint64_t overruns)
{
    if (!page_) return;
    store_release(&page_->ring_overruns, overruns);
    store_release(&page_->ring_reason, reason);
    store_release(&page_->ring_state, static_cast<std::uint32_t>(state));
}

void CaptureHealthWriter::heartbeat()
{
    if (page_) store_release(&page_->heartbeat_mono_ns,
                             sze_recovery::monotonic_time_ns());
}

RecoveryShardHealthWriter::RecoveryShardHealthWriter()
    : fd_(-1), mapping_(0), mapping_bytes_(0U), page_(0), symbols_(0) {}
RecoveryShardHealthWriter::~RecoveryShardHealthWriter() { close(); }

bool RecoveryShardHealthWriter::create(
    const std::string& path, std::uint32_t trading_day, std::uint32_t source_id,
    std::uint64_t generation, std::uint32_t shard_id, std::uint32_t shard_count,
    const std::vector<std::uint32_t>& symbols, bool replace_stale)
{
    close();
    if (shard_count == 0U || shard_id >= shard_count || symbols.empty()) {
        return false;
    }
    std::vector<std::uint8_t> seen(1000000U, 0U);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (symbols[i] >= seen.size() || seen[symbols[i]] != 0U) return false;
        seen[symbols[i]] = 1U;
    }
    if (!prepare_path(path, replace_stale)) return false;
    mapping_bytes_ = kHealthPageBytes + symbols.size() * sizeof(SymbolHealthRecord);
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    if (fd_ < 0 || ::ftruncate(fd_, static_cast<off_t>(mapping_bytes_)) != 0) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        return false;
    }
    void* memory = ::mmap(0, mapping_bytes_, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd_, 0);
    if (memory == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    mapping_ = static_cast<unsigned char*>(memory);
    std::memset(mapping_, 0, mapping_bytes_);
    page_ = reinterpret_cast<RecoveryShardHealthPage*>(mapping_);
    symbols_ = reinterpret_cast<SymbolHealthRecord*>(mapping_ + kHealthPageBytes);
    std::memcpy(page_->magic, kShardMagic, 8);
    page_->version = kHealthFormatVersion;
    page_->header_bytes = kHealthPageBytes;
    page_->trading_day = trading_day;
    page_->source_id = source_id;
    page_->generation = generation;
    page_->writer_pid = static_cast<std::uint32_t>(::getpid());
    page_->shard_id = shard_id;
    page_->shard_count = shard_count;
    page_->readiness = sze_recovery::kReadinessNotReady;
    page_->health_state = kHealthUnknown;
    page_->symbol_count = static_cast<std::uint32_t>(symbols.size());
    page_->symbol_record_bytes = sizeof(SymbolHealthRecord);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        symbols_[i].symbol_id = symbols[i];
        symbols_[i].book_validity = kBookUnknown;
        symbols_[i].prediction_state = kPredictionUnknown;
    }
    symbol_index_.assign(1000000U, -1);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        symbol_index_[symbols[i]] = static_cast<std::int32_t>(i);
    }
    page_->heartbeat_mono_ns = sze_recovery::monotonic_time_ns();
    return true;
}

void RecoveryShardHealthWriter::close()
{
    if (mapping_) ::munmap(mapping_, mapping_bytes_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    mapping_ = 0;
    mapping_bytes_ = 0U;
    page_ = 0;
    symbols_ = 0;
    symbol_index_.clear();
}

void RecoveryShardHealthWriter::publish_shard(
    sze_recovery::ReadinessState readiness, HealthState health,
    std::uint32_t invalid_reason, std::uint64_t global_event_id,
    std::uint64_t shard_event_id, std::uint64_t feed_sequence,
    std::uint64_t replay_lag, std::uint64_t replay_rate_milli,
    std::uint64_t ring_overruns, std::uint64_t events_processed)
{
    if (!page_) return;
    store_release(&page_->last_global_event_id, global_event_id);
    store_release(&page_->last_shard_event_id, shard_event_id);
    store_release(&page_->latest_feed_sequence, feed_sequence);
    store_release(&page_->replay_lag, replay_lag);
    store_release(&page_->replay_rate_milli, replay_rate_milli);
    store_release(&page_->ring_overruns, ring_overruns);
    store_release(&page_->events_processed, events_processed);
    store_release(&page_->invalid_reason, invalid_reason);
    store_release(&page_->health_state, static_cast<std::uint32_t>(health));
    store_release(&page_->readiness, static_cast<std::uint32_t>(readiness));
    store_release(&page_->heartbeat_mono_ns, sze_recovery::monotonic_time_ns());
}

int RecoveryShardHealthWriter::find_symbol(std::uint32_t symbol_id) const
{
    return symbol_id < symbol_index_.size() ? symbol_index_[symbol_id] : -1;
}

bool RecoveryShardHealthWriter::publish_symbol(
    std::uint32_t symbol_id, BookValidity book, PredictionState prediction,
    std::uint32_t book_reason, std::uint32_t prediction_reason,
    std::uint64_t global_event_id, std::uint64_t book_update_mono_ns,
    std::uint64_t prediction_mono_ns, double turnover)
{
    const int index = find_symbol(symbol_id);
    if (index < 0) return false;
    SymbolHealthRecord* record = symbols_ + index;
    std::uint64_t turnover_bits = 0U;
    std::memcpy(&turnover_bits, &turnover, sizeof(turnover_bits));
    store_release(&record->last_global_event_id, global_event_id);
    store_release(&record->last_book_update_mono_ns, book_update_mono_ns);
    store_release(&record->last_prediction_mono_ns, prediction_mono_ns);
    store_release(&record->last_turnover_bits, turnover_bits);
    store_release(&record->book_reason, book_reason);
    store_release(&record->prediction_reason, prediction_reason);
    store_release(&record->book_validity, static_cast<std::uint32_t>(book));
    store_release(&record->prediction_state,
                  static_cast<std::uint32_t>(prediction));
    return true;
}

bool RecoveryShardHealthWriter::publish_book(
    std::uint32_t symbol_id, BookValidity book, std::uint32_t reason,
    std::uint64_t global_event_id, std::uint64_t update_mono_ns)
{
    const int index = find_symbol(symbol_id);
    if (index < 0) return false;
    SymbolHealthRecord* record = symbols_ + index;
    const std::uint32_t old = load_acquire(&record->book_validity);
    const bool old_invalid = old == kBookInvalid ||
        load_acquire(&record->prediction_state) == kPredictionInvalid;
    const bool new_invalid = book == kBookInvalid ||
        load_acquire(&record->prediction_state) == kPredictionInvalid;
    if (old_invalid && !new_invalid) {
        __atomic_sub_fetch(&page_->invalid_symbol_count, 1U, __ATOMIC_RELAXED);
    } else if (!old_invalid && new_invalid) {
        __atomic_add_fetch(&page_->invalid_symbol_count, 1U, __ATOMIC_RELAXED);
    }
    store_release(&record->last_global_event_id, global_event_id);
    store_release(&record->last_book_update_mono_ns, update_mono_ns);
    store_release(&record->book_reason, reason);
    store_release(&record->book_validity, static_cast<std::uint32_t>(book));
    return true;
}

bool RecoveryShardHealthWriter::publish_prediction(
    std::uint32_t symbol_id, PredictionState state, std::uint32_t reason,
    std::uint64_t prediction_mono_ns, double turnover)
{
    const int index = find_symbol(symbol_id);
    if (index < 0) return false;
    SymbolHealthRecord* record = symbols_ + index;
    const std::uint32_t old = load_acquire(&record->prediction_state);
    const bool old_invalid = old == kPredictionInvalid ||
        load_acquire(&record->book_validity) == kBookInvalid;
    const bool new_invalid = state == kPredictionInvalid ||
        load_acquire(&record->book_validity) == kBookInvalid;
    if (old_invalid && !new_invalid) {
        __atomic_sub_fetch(&page_->invalid_symbol_count, 1U, __ATOMIC_RELAXED);
    } else if (!old_invalid && new_invalid) {
        __atomic_add_fetch(&page_->invalid_symbol_count, 1U, __ATOMIC_RELAXED);
    }
    std::uint64_t turnover_bits = 0U;
    std::memcpy(&turnover_bits, &turnover, sizeof(turnover_bits));
    store_release(&record->last_prediction_mono_ns, prediction_mono_ns);
    store_release(&record->last_turnover_bits, turnover_bits);
    store_release(&record->prediction_reason, reason);
    store_release(&record->prediction_state, static_cast<std::uint32_t>(state));
    return true;
}

bool RecoveryShardHealthWriter::publish_book_valid_once(
    std::uint32_t symbol_id, std::uint64_t global_event_id,
    std::uint64_t update_mono_ns)
{
    const int index = find_symbol(symbol_id);
    if (index < 0) return false;
    if (load_acquire(&symbols_[index].book_validity) != kBookUnknown) {
        return true;
    }
    return publish_book(symbol_id, kBookValid, 0U, global_event_id,
                        update_mono_ns);
}

TdHealthWriter::TdHealthWriter() : fd_(-1), page_(0) {}
TdHealthWriter::~TdHealthWriter() { close(); }

bool TdHealthWriter::create(const std::string& path, std::uint32_t trading_day,
                            std::uint32_t source_id, std::uint64_t generation,
                            bool replace_stale)
{
    close();
    if (!create_page(path, replace_stale, kTdMagic, &fd_, &page_)) return false;
    page_->trading_day = trading_day;
    page_->source_id = source_id;
    page_->generation = generation;
    page_->td_state = kTdUnknown;
    page_->heartbeat_mono_ns = sze_recovery::monotonic_time_ns();
    return true;
}

void TdHealthWriter::close()
{
    if (page_) ::munmap(page_, kHealthPageBytes);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    page_ = 0;
}

void TdHealthWriter::publish(TdState state, bool logged_in, bool account_ready,
                             bool positions_ready, std::uint32_t last_error,
                             std::uint64_t account_query_ns,
                             std::uint64_t position_query_ns,
                             std::uint64_t order_callback_ns,
                             std::uint64_t rejected_orders)
{
    if (!page_) return;
    store_release(&page_->account_query_mono_ns, account_query_ns);
    store_release(&page_->position_query_mono_ns, position_query_ns);
    store_release(&page_->last_order_callback_mono_ns, order_callback_ns);
    store_release(&page_->rejected_orders, rejected_orders);
    store_release(&page_->logged_in, logged_in ? 1U : 0U);
    store_release(&page_->account_ready, account_ready ? 1U : 0U);
    store_release(&page_->positions_ready, positions_ready ? 1U : 0U);
    store_release(&page_->last_error, last_error);
    store_release(&page_->td_state, static_cast<std::uint32_t>(state));
    store_release(&page_->heartbeat_mono_ns, sze_recovery::monotonic_time_ns());
}

HealthReader::HealthReader()
    : fd_(-1), mapping_(0), mapping_bytes_(0U), kind_(kNone) {}
HealthReader::~HealthReader() { close(); }

bool HealthReader::map(const std::string& path, Kind kind)
{
    close();
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat info;
    if (fd_ < 0 || ::fstat(fd_, &info) != 0 ||
        info.st_size < static_cast<off_t>(kHealthPageBytes)) {
        close();
        return false;
    }
    mapping_bytes_ = static_cast<std::size_t>(info.st_size);
    void* memory = ::mmap(0, mapping_bytes_, PROT_READ, MAP_SHARED, fd_, 0);
    if (memory == MAP_FAILED) {
        mapping_ = 0;
        close();
        return false;
    }
    mapping_ = static_cast<const unsigned char*>(memory);
    kind_ = kind;
    const char* expected = kind == kCapture ? kCaptureMagic :
        (kind == kShard ? kShardMagic : kTdMagic);
    if (std::memcmp(mapping_, expected, 8) != 0) {
        close();
        return false;
    }
    const std::uint32_t version = kind == kCapture
        ? reinterpret_cast<const CaptureHealthPage*>(mapping_)->version
        : (kind == kShard
           ? reinterpret_cast<const RecoveryShardHealthPage*>(mapping_)->version
           : reinterpret_cast<const TdHealthPage*>(mapping_)->version);
    const std::uint32_t header_bytes = kind == kCapture
        ? reinterpret_cast<const CaptureHealthPage*>(mapping_)->header_bytes
        : (kind == kShard
           ? reinterpret_cast<const RecoveryShardHealthPage*>(mapping_)->header_bytes
           : reinterpret_cast<const TdHealthPage*>(mapping_)->header_bytes);
    if (version != kHealthFormatVersion || header_bytes != kHealthPageBytes) {
        close();
        return false;
    }
    if (kind == kShard) {
        const RecoveryShardHealthPage* page = shard();
        const std::size_t expected_bytes = kHealthPageBytes +
            static_cast<std::size_t>(page->symbol_count) * sizeof(SymbolHealthRecord);
        if (page->symbol_record_bytes != sizeof(SymbolHealthRecord) ||
            expected_bytes != mapping_bytes_) {
            close();
            return false;
        }
    }
    return true;
}

bool HealthReader::open_capture(const std::string& path) { return map(path, kCapture); }
bool HealthReader::open_shard(const std::string& path) { return map(path, kShard); }
bool HealthReader::open_td(const std::string& path) { return map(path, kTd); }

void HealthReader::close()
{
    if (mapping_) ::munmap(const_cast<unsigned char*>(mapping_), mapping_bytes_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    mapping_ = 0;
    mapping_bytes_ = 0U;
    kind_ = kNone;
}

const CaptureHealthPage* HealthReader::capture() const
{
    return kind_ == kCapture ? reinterpret_cast<const CaptureHealthPage*>(mapping_) : 0;
}
const RecoveryShardHealthPage* HealthReader::shard() const
{
    return kind_ == kShard ? reinterpret_cast<const RecoveryShardHealthPage*>(mapping_) : 0;
}
const TdHealthPage* HealthReader::td() const
{
    return kind_ == kTd ? reinterpret_cast<const TdHealthPage*>(mapping_) : 0;
}

const SymbolHealthRecord* HealthReader::symbol(std::uint32_t symbol_id) const
{
    const RecoveryShardHealthPage* header = shard();
    if (!header) return 0;
    const SymbolHealthRecord* records = reinterpret_cast<const SymbolHealthRecord*>(
        mapping_ + kHealthPageBytes);
    for (std::uint32_t i = 0; i < header->symbol_count; ++i) {
        if (records[i].symbol_id == symbol_id) return records + i;
    }
    return 0;
}

TradeDecision derive_can_trade(const CaptureHealthPage& capture,
                               const RecoveryShardHealthPage& shard,
                               const SymbolHealthRecord* symbol,
                               const TdHealthPage& td,
                               const TradePolicy& policy)
{
    TradeDecision result;
    if (capture.trading_day != shard.trading_day ||
        capture.trading_day != td.trading_day ||
        capture.generation != shard.generation ||
        !health_writer_alive(capture.writer_pid) ||
        !health_writer_alive(shard.writer_pid) ||
        !health_writer_alive(td.writer_pid) ||
        !health_heartbeat_fresh(capture.heartbeat_mono_ns) ||
        !health_heartbeat_fresh(shard.heartbeat_mono_ns) ||
        !health_heartbeat_fresh(td.heartbeat_mono_ns) ||
        capture.feed_state != kHealthHealthy) {
        result.reason = kTradeFeedNotHealthy;
        return result;
    }
    if (capture.ring_state != kHealthHealthy) {
        result.reason = kTradeRingNotHealthy;
        return result;
    }
    if (shard.health_state != kHealthHealthy ||
        shard.readiness != sze_recovery::kReadinessLiveReady ||
        shard.ring_overruns != 0U) {
        result.reason = kTradeShardNotLive;
        return result;
    }
    if (!symbol) {
        result.reason = kTradeSymbolNotOwned;
        return result;
    }
    if (symbol->book_validity != kBookValid) {
        result.reason = kTradeBookInvalid;
        return result;
    }
    if (symbol->prediction_state != kPredictionHealthy) {
        result.reason = kTradePredictionInvalid;
        return result;
    }
    if (td.td_state != kTdReady || !td.logged_in || !td.account_ready ||
        !td.positions_ready) {
        result.reason = kTradeTdNotReady;
        return result;
    }
    result.allow_risk_reduction = true;
    if (policy.require_durable_journal_for_new_risk &&
        capture.journal_state != kHealthHealthy) {
        result.reason = kTradeJournalDegraded;
        return result;
    }
    result.allow_new_risk = true;
    result.reason = kTradeAllowed;
    return result;
}

bool health_writer_alive(std::uint32_t pid)
{
    return pid_alive(pid);
}

bool health_heartbeat_fresh(std::uint64_t heartbeat_mono_ns,
                            std::uint64_t max_age_ns)
{
    const std::uint64_t now = sze_recovery::monotonic_time_ns();
    return heartbeat_mono_ns > 0U && now >= heartbeat_mono_ns &&
        now - heartbeat_mono_ns <= max_age_ns;
}

const char* health_state_name(HealthState state)
{
    switch (state) {
    case kHealthUnknown: return "unknown";
    case kHealthHealthy: return "healthy";
    case kHealthDegraded: return "degraded";
    case kHealthFailed: return "failed";
    }
    return "invalid";
}

const char* trade_block_reason_name(TradeBlockReason reason)
{
    switch (reason) {
    case kTradeAllowed: return "allowed";
    case kTradeFeedNotHealthy: return "feed_not_healthy";
    case kTradeRingNotHealthy: return "ring_not_healthy";
    case kTradeShardNotLive: return "shard_not_live";
    case kTradeBookInvalid: return "book_invalid";
    case kTradePredictionInvalid: return "prediction_invalid";
    case kTradeTdNotReady: return "td_not_ready";
    case kTradeJournalDegraded: return "journal_degraded";
    case kTradeSymbolNotOwned: return "symbol_not_owned";
    }
    return "unknown";
}

std::uint32_t parse_symbol_id(const char* symbol)
{
    if (!symbol) return 1000000U;
    std::uint32_t value = 0U;
    for (std::size_t i = 0; i < 6U; ++i) {
        const unsigned char digit = static_cast<unsigned char>(symbol[i]);
        if (digit < '0' || digit > '9') return 1000000U;
        value = value * 10U + static_cast<std::uint32_t>(digit - '0');
    }
    return value;
}

std::string capture_health_path(const std::string& ring_path)
{
    return ring_path + ".health";
}

std::string shard_health_path(const std::string& ring_path,
                              std::uint32_t shard_id)
{
    std::ostringstream value;
    value << ring_path << ".shard." << shard_id << ".health";
    return value.str();
}

std::string td_health_path(const std::string& ring_path)
{
    return ring_path + ".td.health";
}

}  // namespace sze_health
