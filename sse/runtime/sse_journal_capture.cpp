#include "sse/runtime/sse_journal_capture.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <limits>
#include <pthread.h>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <time.h>

namespace sse_journal_capture {
namespace {

const std::uint64_t kMaxQueueCapacity = 1ULL << 20U;

std::string status_message(const char* operation, sze_recovery::JournalStatus status) {
    return std::string("Shanghai journal ") + operation + " failed (status " +
        std::to_string(static_cast<int>(status)) + ')';
}

bool first_segment_exists(const sze_recovery::JournalConfig& config, std::string* error) {
    char name[128];
    const int written = std::snprintf(name, sizeof(name), "%s_%08u_s%u_%06u.szej",
                                      config.prefix.empty() ? "sze" : config.prefix.c_str(),
                                      config.trading_day, config.source_id, 0U);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(name)) {
        if (error) *error = "Shanghai journal segment name is too long";
        return true;
    }
    std::string path = config.directory;
    if (!path.empty() && path[path.size() - 1U] != '/') path.push_back('/');
    path += name;
    struct stat info;
    if (::stat(path.c_str(), &info) == 0) return true;
    if (errno != ENOENT) {
        if (error) *error = "cannot inspect existing Shanghai journal segment";
        return true;
    }
    return false;
}

}  // namespace

namespace {

typedef nlohmann::json Json;

std::uint64_t monotonic_now_ns() {
    timespec value = {};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(value.tv_nsec);
}

class StopSignals {
public:
    StopSignals(deepwin_market_data::MarketDataStream* stream, Capture* capture)
        : stream_(stream), capture_(capture), done_(false) {
        sigemptyset(&signals_);
        sigaddset(&signals_, SIGINT);
        sigaddset(&signals_, SIGTERM);
        if (::pthread_sigmask(SIG_BLOCK, &signals_, &old_) != 0)
            throw std::runtime_error("cannot configure Shanghai capture stop signals");
        try {
            thread_ = std::thread([this]() { monitor(); });
        } catch (...) {
            ::pthread_sigmask(SIG_SETMASK, &old_, 0);
            throw;
        }
    }

    ~StopSignals() {
        done_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        ::pthread_sigmask(SIG_SETMASK, &old_, 0);
    }

private:
    void monitor() {
        bool requested = false;
        bool announced_ready = false;
        std::uint64_t next_status_ns = monotonic_now_ns() + 5000000000ULL;
        while (!done_.load(std::memory_order_acquire)) {
            timespec wait = {0, 10000000L};
            const int signal = ::sigtimedwait(&signals_, 0, &wait);
            if (signal == SIGINT || signal == SIGTERM) requested = true;
            if (capture_->failed()) requested = true;
            if (requested) {
                capture_->request_stop();
                stream_->stop();
            }
            const sse_journal_capture::Status status = capture_->status();
            if (!announced_ready && stream_->ready() && status.journal_ready) {
                std::cerr << "Shanghai journal capture ready\n";
                announced_ready = true;
            }
            const std::uint64_t now = monotonic_now_ns();
            if (now >= next_status_ns) {
                const deepwin_market_data::StreamHealth health = stream_->health();
                Json line;
                line["event"] = "capture_status";
                line["ready"] = stream_->ready() && status.journal_ready && !status.failed;
                line["datagrams"] = status.datagrams;
                line["hardware_timestamps"] = status.hardware_timestamps;
                line["missing_hardware_timestamps"] = status.missing_hardware_timestamps;
                line["software_timestamp_fallbacks"] = status.software_timestamp_fallbacks;
                line["payload_bytes"] = status.payload_bytes;
                line["last_receive_ns"] = status.last_receive_ns;
                line["accepted_events"] = status.accepted_events;
                line["journal_events"] = status.journal_events;
                line["journal_errors"] = status.journal_errors;
                line["journal_overflows"] = status.journal_overflows;
                line["queue_size"] = status.queue_size;
                line["recording_failed"] = health.recording_failed;
                line["input_valid"] = health.input_valid;
                line["processing_valid"] = health.processing_valid && !status.failed;
                line["journal_degraded"] = status.journal_degraded;
                line["stop_requested"] = status.stop_requested;
                std::cerr << line.dump() << "\n";
                next_status_ns = now + 5000000000ULL;
            }
        }
    }

    deepwin_market_data::MarketDataStream* stream_;
    Capture* capture_;
    sigset_t signals_, old_;
    std::atomic<bool> done_;
    std::thread thread_;
};

void append_cpu_affinity(Json* output, const std::vector<sse_cpu::Cpu>& cpus) {
    (*output)["cpu_affinity"] = Json::array();
    const char* roles[] = {"receive", "dispatch", "journal"};
    for (std::size_t index = 0; index < cpus.size() && index < 3U; ++index) {
        (*output)["cpu_affinity"].push_back(Json{{"role", roles[index]},
            {"cpu", cpus[index].id}, {"l3", cpus[index].l3}});
    }
}

}  // namespace

class Capture::Queue {
public:
    explicit Queue(std::size_t capacity)
        : slots_(capacity), capacity_(capacity), head_(0), tail_(0), high_water_(0) {}

    bool reserve(sse_journal::StoredEvent** slot, std::uint64_t* ticket) {
        if (!slot || !ticket || !capacity_) return false;
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) return false;
        *slot = &slots_[static_cast<std::size_t>(head % capacity_)];
        *ticket = head;
        return true;
    }

    void commit(std::uint64_t ticket) {
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::uint64_t size = ticket - tail + 1U;
        std::uint64_t previous = high_water_.load(std::memory_order_relaxed);
        while (size > previous &&
               !high_water_.compare_exchange_weak(previous, size,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
        }
        head_.store(ticket + 1U, std::memory_order_release);
    }

    bool front(const sse_journal::StoredEvent** slot) const {
        if (!slot) return false;
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        *slot = &slots_[static_cast<std::size_t>(tail % capacity_)];
        return true;
    }

    void pop() {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        tail_.store(tail + 1U, std::memory_order_release);
    }

    std::uint64_t size() const {
        return head_.load(std::memory_order_acquire) -
            tail_.load(std::memory_order_acquire);
    }

    std::uint64_t high_water() const {
        return high_water_.load(std::memory_order_relaxed);
    }

private:
    std::vector<sse_journal::StoredEvent> slots_;
    std::size_t capacity_;
    std::atomic<std::uint64_t> head_;
    std::atomic<std::uint64_t> tail_;
    std::atomic<std::uint64_t> high_water_;
};

Capture::Capture(const sse_journal::Config& config)
    : config_(config), queue_(), writer_(), ring_(), lease_(), cpu_affinity_(), journal_thread_(),
      open_(false), accepting_(false), stop_requested_(false), failed_(false),
      finish_requested_(false),
      journal_degraded_(false), journal_ready_(false), journal_start_failed_(false),
      clean_shutdown_requested_(false), journal_closed_(false),
      failure_reason_(static_cast<int>(sze_recovery::kInvalidNone)),
      datagrams_(0), hardware_timestamps_(0), missing_hardware_timestamps_(0),
      software_timestamp_fallbacks_(0), idle_events_(0), payload_bytes_(0), last_receive_ns_(0),
      accepted_events_(0), journal_events_(0), journal_errors_(0), journal_overflows_(0),
      latest_event_id_(0), latest_feed_sequence_(0), journal_published_offset_(0),
      journal_flushed_offset_(0), flush_count_(0), error_mutex_(), error_() {
}

Capture::~Capture() {
    if (is_open()) {
        std::string ignored;
        finish(false, &ignored);
    } else if (journal_thread_.joinable()) {
        journal_thread_.join();
    }
}

bool Capture::open(std::string* error) {
    if (error) error->clear();
    if (is_open()) {
        if (error) *error = "Shanghai journal capture is already open";
        return false;
    }
    if (config_.journal.source_id != 89U || config_.ring.source_id != 89U ||
        config_.journal.trading_day != config_.ring.trading_day ||
        config_.journal.generation == 0U || config_.ring.generation != config_.journal.generation) {
        if (error) *error = "Shanghai journal source/day/generation identity is invalid";
        return false;
    }
    if (config_.journal.max_payload_bytes != sse_journal::stored_header_bytes(config_) + sse_journal::kMaxDatagram ||
        config_.ring.max_payload_bytes != config_.journal.max_payload_bytes ||
        config_.stream.max_datagram_bytes == 0U ||
        config_.stream.max_datagram_bytes > sse_journal::kMaxDatagram ||
        (config_.schema_version == 1U && config_.stream.idle_gap_ns != 100000U) ||
        (config_.schema_version == 2U &&
         (!config_.stream.idle_gap_ns || config_.stream.idle_gap_ns > 1000000000ULL ||
          !config_.extended_timestamps || config_.stream.hardware_timestamp_interface.empty())) ||
        !config_.journal_queue_capacity || config_.journal_queue_capacity > kMaxQueueCapacity ||
        config_.journal_queue_capacity >
            (std::numeric_limits<std::size_t>::max)() / sizeof(sse_journal::StoredEvent)) {
        if (error) *error = "invalid Shanghai journal transport limits";
        return false;
    }
    if (!config_.stream.recording_directory.empty() || config_.stream.recording_required) {
        if (error) *error = "Shanghai journal capture must disable T0MD recording";
        return false;
    }
    if (config_.channels.empty()) {
        if (error) *error = "Shanghai journal capture requires at least one channel";
        return false;
    }
    if (first_segment_exists(config_.journal, error)) {
        if (error && error->empty()) *error = "Shanghai journal epoch already exists";
        return false;
    }

    failed_.store(false, std::memory_order_release);
    finish_requested_.store(false, std::memory_order_release);
    journal_degraded_.store(false, std::memory_order_release);
    journal_ready_.store(false, std::memory_order_release);
    journal_start_failed_.store(false, std::memory_order_release);
    clean_shutdown_requested_.store(false, std::memory_order_release);
    journal_closed_.store(false, std::memory_order_release);
    failure_reason_.store(static_cast<int>(sze_recovery::kInvalidNone), std::memory_order_release);
    datagrams_.store(0, std::memory_order_relaxed);
    hardware_timestamps_.store(0, std::memory_order_relaxed);
    missing_hardware_timestamps_.store(0, std::memory_order_relaxed);
    software_timestamp_fallbacks_.store(0, std::memory_order_relaxed);
    idle_events_.store(0, std::memory_order_relaxed);
    payload_bytes_.store(0, std::memory_order_relaxed);
    last_receive_ns_.store(0, std::memory_order_relaxed);
    accepted_events_.store(0, std::memory_order_relaxed);
    journal_events_.store(0, std::memory_order_relaxed);
    journal_errors_.store(0, std::memory_order_relaxed);
    journal_overflows_.store(0, std::memory_order_relaxed);
    latest_event_id_.store(0, std::memory_order_relaxed);
    latest_feed_sequence_.store(0, std::memory_order_relaxed);
    journal_published_offset_.store(0, std::memory_order_relaxed);
    journal_flushed_offset_.store(0, std::memory_order_relaxed);
    flush_count_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_.clear();
    }
    queue_.reset(new Queue(config_.journal_queue_capacity));

    std::string lease_error;
    const std::vector<int> requested = {
        config_.stream.receive_cpu, config_.stream.dispatch_cpu, config_.journal_cpu};
    if (!lease_.acquire(requested, &lease_error)) {
        if (error) *error = lease_error.empty() ? "Shanghai CPU lease failed" : lease_error;
        return false;
    }
    if (lease_.cpus().size() != 3U) {
        lease_.release();
        if (error) *error = "Shanghai CPU lease returned an invalid role count";
        return false;
    }
    cpu_affinity_.assign(lease_.cpus().begin(), lease_.cpus().end());
    config_.stream.receive_cpu = cpu_affinity_[0].id;
    config_.stream.dispatch_cpu = cpu_affinity_[1].id;
    config_.journal_cpu = cpu_affinity_[2].id;

    const sze_recovery::JournalOpenResult journal = writer_.open(config_.journal);
    if (journal.status != sze_recovery::kJournalOk || journal.existing ||
        journal.unclean_restart || journal.corrupt_tail) {
        writer_.close(false);
        lease_.release();
        if (error) {
            *error = journal.status != sze_recovery::kJournalOk
                ? status_message("open", journal.status)
                : "Shanghai journal must be a new clean epoch";
        }
        return false;
    }
    if (!ring_.create(config_.ring)) {
        writer_.close(false);
        lease_.release();
        if (error) *error = "Shanghai SHM ring create failed";
        return false;
    }
    ring_.publish_state(sze_recovery::kContinuityInitializing,
                        sze_recovery::kReadinessNotReady,
                        sze_recovery::kInvalidNone, 0U, 0U);
    open_.store(true, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);
    try {
        journal_thread_ = std::thread(&Capture::journal_loop, this);
    } catch (const std::exception& exception) {
        accepting_.store(false, std::memory_order_release);
        open_.store(false, std::memory_order_release);
        ring_.publish_continuity(sze_recovery::kContinuityInvalid,
                                 sze_recovery::kInvalidReceiverStopped, 0U, 0U);
        ring_.set_readiness(sze_recovery::kReadinessNotReady);
        ring_.close();
        writer_.close(false);
        lease_.release();
        if (error) *error = exception.what();
        return false;
    }
    while (!journal_ready_.load(std::memory_order_acquire) &&
           !journal_start_failed_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    if (journal_start_failed_.load(std::memory_order_acquire)) {
        if (journal_thread_.joinable()) journal_thread_.join();
        const std::string message = this->error();
        open_.store(false, std::memory_order_release);
        writer_.close(false);
        ring_.close();
        lease_.release();
        if (error) *error = message.empty() ? "Shanghai journal worker startup failed" : message;
        return false;
    }
    return true;
}

bool Capture::on_event(const deepwin_market_data::StreamEvent& event, std::string* error) {
    if (error) error->clear();
    if (!is_open()) {
        if (error) *error = "Shanghai journal capture is not open";
        return false;
    }
    if (event.kind == deepwin_market_data::kDatagramEvent) {
        datagrams_.fetch_add(1U, std::memory_order_relaxed);
        if (event.timestamp_flags & deepwin_market_data::kHardwareReceiveTimestamp)
            hardware_timestamps_.fetch_add(1U, std::memory_order_relaxed);
        else if (event.timestamp_flags & deepwin_market_data::kHardwareTimestampRequested)
            missing_hardware_timestamps_.fetch_add(1U, std::memory_order_relaxed);
        if (event.timestamp_flags & deepwin_market_data::kUserspaceRealtimeTimestamp)
            software_timestamp_fallbacks_.fetch_add(1U, std::memory_order_relaxed);
        payload_bytes_.fetch_add(static_cast<std::uint64_t>(event.size), std::memory_order_relaxed);
        last_receive_ns_.store(event.monotonic_ns, std::memory_order_relaxed);
    } else if (event.kind == deepwin_market_data::kIdleEvent) {
        idle_events_.fetch_add(1U, std::memory_order_relaxed);
    }
    if (failed_.load(std::memory_order_acquire)) {
        if (error) *error = this->error();
        return false;
    }

    sse_journal::StoredEvent* slot = 0;
    std::uint64_t ticket = 0;
    if (!queue_ || !queue_->reserve(&slot, &ticket)) {
        journal_overflows_.fetch_add(1U, std::memory_order_relaxed);
        fail("Shanghai journal SPSC queue overflow", sze_recovery::kInvalidRingOverrun,
             event.sequence, event.sequence);
        if (error) *error = this->error();
        return false;
    }
    try {
        sse_journal::encode(event, config_, slot);
    } catch (const std::exception& exception) {
        fail(exception.what(), sze_recovery::kInvalidMalformedRecord,
             event.sequence, event.sequence);
        if (error) *error = this->error();
        return false;
    } catch (...) {
        fail("unknown Shanghai stream event encoding failure",
             sze_recovery::kInvalidMalformedRecord, event.sequence, event.sequence);
        if (error) *error = this->error();
        return false;
    }
    if (!ring_.publish(slot->event, slot->payload)) {
        fail("Shanghai SHM ring publish failed", sze_recovery::kInvalidRingOverrun,
             slot->event.event_id, slot->event.feed_sequence);
        if (error) *error = this->error();
        return false;
    }
    queue_->commit(ticket);
    accepted_events_.fetch_add(1U, std::memory_order_relaxed);
    latest_event_id_.store(slot->event.event_id, std::memory_order_relaxed);
    latest_feed_sequence_.store(slot->event.feed_sequence, std::memory_order_relaxed);
    ring_.publish_capture_metrics(
        accepted_events_.load(std::memory_order_relaxed),
        accepted_events_.load(std::memory_order_relaxed), 0U, 0U,
        journal_overflows_.load(std::memory_order_relaxed));
    return true;
}

void Capture::request_stop() {
    stop_requested_.store(true, std::memory_order_release);
}

bool Capture::finish(bool clean_shutdown, std::string* error) {
    if (error) error->clear();
    if (!is_open()) {
        if (error && failed()) *error = this->error();
        return !failed();
    }
    clean_shutdown_requested_.store(clean_shutdown && !failed(), std::memory_order_release);
    finish_requested_.store(true, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    if (journal_thread_.joinable()) journal_thread_.join();

    const std::uint64_t event_id = latest_event_id_.load(std::memory_order_acquire);
    const std::uint64_t feed_sequence = latest_feed_sequence_.load(std::memory_order_acquire);
    const bool invalid = failed_.load(std::memory_order_acquire) || !clean_shutdown;
    const int stored_reason = failure_reason_.load(std::memory_order_acquire);
    const sze_recovery::InvalidReason reason = invalid && stored_reason !=
        static_cast<int>(sze_recovery::kInvalidNone)
        ? static_cast<sze_recovery::InvalidReason>(stored_reason)
        : sze_recovery::kInvalidReceiverStopped;
    // Journal continuity remains valid after a clean drain. The live ring is
    // deliberately invalid/not-ready so a consumer cannot switch to a stopped
    // producer while the offline journal remains replayable.
    ring_.publish_continuity(sze_recovery::kContinuityInvalid, reason,
                             event_id, feed_sequence);
    ring_.set_readiness(sze_recovery::kReadinessNotReady);
    publish_storage_metrics();
    ring_.close();
    lease_.release();
    open_.store(false, std::memory_order_release);
    if (error && failed()) *error = this->error();
    return !failed();
}

void Capture::journal_loop() {
    std::string bind_error;
    if (!sse_cpu::bind_current_thread(config_.journal_cpu, &bind_error)) {
        journal_start_failed_.store(true, std::memory_order_release);
        fail(bind_error, sze_recovery::kInvalidReceiverStopped, 0U, 0U);
        journal_closed_.store(writer_.close(false) == sze_recovery::kJournalOk,
                              std::memory_order_release);
        return;
    }
    const sze_recovery::JournalStatus continuity = writer_.publish_continuity(
        sze_recovery::kContinuityValid, sze_recovery::kInvalidNone, 0U);
    if (continuity != sze_recovery::kJournalOk) {
        journal_start_failed_.store(true, std::memory_order_release);
        ++journal_errors_;
        fail(status_message("continuity", continuity), sze_recovery::kInvalidJournalCorruption, 0U, 0U);
        journal_closed_.store(writer_.close(false) == sze_recovery::kJournalOk,
                              std::memory_order_release);
        return;
    }
    ring_.publish_state(sze_recovery::kContinuityValid,
                        sze_recovery::kReadinessLiveReady,
                        sze_recovery::kInvalidNone, 0U, 0U);
    journal_ready_.store(true, std::memory_order_release);

    std::uint64_t next_flush_ns = 0U;
    for (;;) {
        const sse_journal::StoredEvent* slot = 0;
        if (queue_ && queue_->front(&slot)) {
            sze_recovery::CanonicalEvent event = slot->event;
            const sze_recovery::JournalStatus status = writer_.append(&event, slot->payload);
            if (status != sze_recovery::kJournalOk) {
                ++journal_errors_;
                fail(status_message("append", status), sze_recovery::kInvalidJournalCorruption,
                     slot->event.event_id, slot->event.feed_sequence);
                break;
            }
            queue_->pop();
            journal_events_.fetch_add(1U, std::memory_order_relaxed);
            const std::uint64_t now = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            if (!next_flush_ns) {
                next_flush_ns = now + static_cast<std::uint64_t>(config_.flush_interval_ms) * 1000000ULL;
            } else if (now >= next_flush_ns) {
                const sze_recovery::JournalStatus flush = writer_.flush(false);
                if (flush != sze_recovery::kJournalOk) {
                    ++journal_errors_;
                    fail(status_message("flush", flush), sze_recovery::kInvalidJournalCorruption,
                         event.event_id, event.feed_sequence);
                    break;
                }
                next_flush_ns = now + static_cast<std::uint64_t>(config_.flush_interval_ms) * 1000000ULL;
            }
            publish_storage_metrics();
            continue;
        }
        if (finish_requested_.load(std::memory_order_acquire) ||
            failed_.load(std::memory_order_acquire)) break;
        std::this_thread::yield();
    }

    const bool clean = clean_shutdown_requested_.load(std::memory_order_acquire) &&
                       !failed_.load(std::memory_order_acquire);
    if (clean) {
        const sze_recovery::JournalStatus flush = writer_.flush(true);
        if (flush != sze_recovery::kJournalOk) {
            ++journal_errors_;
            fail(status_message("final flush", flush), sze_recovery::kInvalidJournalCorruption,
                 latest_event_id_.load(std::memory_order_acquire),
                 latest_feed_sequence_.load(std::memory_order_acquire));
        }
    }
    publish_storage_metrics();
    const bool close_clean = clean && !failed_.load(std::memory_order_acquire);
    const sze_recovery::JournalStatus closed = writer_.close(close_clean);
    if (closed != sze_recovery::kJournalOk) {
        ++journal_errors_;
        fail(status_message("close", closed), sze_recovery::kInvalidJournalCorruption,
             latest_event_id_.load(std::memory_order_acquire),
             latest_feed_sequence_.load(std::memory_order_acquire));
    }
    journal_closed_.store(closed == sze_recovery::kJournalOk, std::memory_order_release);
    publish_storage_metrics();
}

void Capture::fail(const std::string& message, sze_recovery::InvalidReason reason,
                   std::uint64_t event_id, std::uint64_t feed_sequence) {
    fail_locked(message);
    int expected = static_cast<int>(sze_recovery::kInvalidNone);
    failure_reason_.compare_exchange_strong(
        expected, static_cast<int>(reason), std::memory_order_release,
        std::memory_order_relaxed);
    failed_.store(true, std::memory_order_release);
    journal_degraded_.store(true, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    if (ring_.is_open()) {
        ring_.set_journal_degraded(true);
        ring_.publish_continuity(sze_recovery::kContinuityInvalid, reason,
                                 event_id, feed_sequence);
        ring_.set_readiness(sze_recovery::kReadinessNotReady);
    }
}

void Capture::fail_locked(const std::string& message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (error_.empty()) error_ = message;
}

void Capture::publish_storage_metrics() {
    if (!ring_.is_open()) return;
    std::uint64_t published = journal_published_offset_.load(std::memory_order_relaxed);
    std::uint64_t flushed = journal_flushed_offset_.load(std::memory_order_relaxed);
    std::uint64_t flush_count = flush_count_.load(std::memory_order_relaxed);
    if (writer_.is_open()) {
        published = writer_.published_offset();
        flushed = writer_.flushed_offset();
        flush_count = writer_.flush_count();
        journal_published_offset_.store(published, std::memory_order_relaxed);
        journal_flushed_offset_.store(flushed, std::memory_order_relaxed);
        flush_count_.store(flush_count, std::memory_order_relaxed);
    }
    ring_.publish_storage_metrics(
        journal_events_.load(std::memory_order_relaxed),
        journal_errors_.load(std::memory_order_relaxed), flush_count, published, flushed);
}

Status Capture::status() const {
    Status result;
    result.accepting = accepting_.load(std::memory_order_acquire);
    result.stop_requested = stop_requested_.load(std::memory_order_acquire);
    result.failed = failed_.load(std::memory_order_acquire);
    result.journal_degraded = journal_degraded_.load(std::memory_order_acquire);
    result.journal_closed = journal_closed_.load(std::memory_order_acquire);
    result.journal_ready = journal_ready_.load(std::memory_order_acquire);
    result.datagrams = datagrams_.load(std::memory_order_relaxed);
    result.hardware_timestamps = hardware_timestamps_.load(std::memory_order_relaxed);
    result.missing_hardware_timestamps = missing_hardware_timestamps_.load(std::memory_order_relaxed);
    result.software_timestamp_fallbacks = software_timestamp_fallbacks_.load(std::memory_order_relaxed);
    result.idle_events = idle_events_.load(std::memory_order_relaxed);
    result.payload_bytes = payload_bytes_.load(std::memory_order_relaxed);
    result.last_receive_ns = last_receive_ns_.load(std::memory_order_relaxed);
    result.accepted_events = accepted_events_.load(std::memory_order_relaxed);
    result.journal_events = journal_events_.load(std::memory_order_relaxed);
    result.journal_errors = journal_errors_.load(std::memory_order_relaxed);
    result.journal_overflows = journal_overflows_.load(std::memory_order_relaxed);
    result.queue_size = queue_ ? queue_->size() : 0U;
    result.queue_high_water = queue_ ? queue_->high_water() : 0U;
    result.latest_event_id = latest_event_id_.load(std::memory_order_relaxed);
    result.latest_feed_sequence = latest_feed_sequence_.load(std::memory_order_relaxed);
    result.journal_published_offset = journal_published_offset_.load(std::memory_order_relaxed);
    result.journal_flushed_offset = journal_flushed_offset_.load(std::memory_order_relaxed);
    result.flush_count = flush_count_.load(std::memory_order_relaxed);
    result.cpu_affinity = cpu_affinity_;
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        result.error = error_;
    }
    return result;
}

std::string Capture::error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

int run_cli(const std::string& config_path) {
    try {
        sse_journal::Config config = sse_journal::load(config_path);
        // Journal workers are created by Capture::open before StopSignals.
        // Block termination first so every worker inherits the mask and only
        // the signal waiter initiates a clean drain, including SIGTERM.
        sigset_t stop_set;
        sigemptyset(&stop_set);
        sigaddset(&stop_set, SIGINT);
        sigaddset(&stop_set, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &stop_set, 0) != 0)
            throw std::runtime_error("cannot block capture stop signals");
        deepwin_market_data::MarketDataStream stream;
        Capture capture(config);
        std::string error;
        if (!capture.open(&error)) {
            std::cerr << (error.empty() ? "Shanghai journal capture open failed" : error) << "\n";
            return 1;
        }
        if (capture.cpu_affinity().size() != 3U) {
            std::cerr << "Shanghai CPU lease returned an invalid role count\n";
            capture.finish(false, &error);
            return 1;
        }
        config.stream.receive_cpu = capture.cpu_affinity()[0].id;
        config.stream.dispatch_cpu = capture.cpu_affinity()[1].id;
        deepwin_market_data::StreamCallback callback =
            [&stream, &capture](const deepwin_market_data::StreamEvent& event) {
                std::string callback_error;
                if (!capture.on_event(event, &callback_error)) {
                    if (!callback_error.empty()) std::cerr << callback_error << "\n";
                    stream.stop();
                }
            };
        bool stream_ok = false;
        std::string stream_error;
        {
            StopSignals stop_signals(&stream, &capture);
            stream_ok = stream.run(config.channels, config.stream, callback,
                                   config.duration_ms, &stream_error);
        }
        const bool clean = stream_ok && !capture.failed();
        std::string finish_error;
        const bool capture_ok = capture.finish(clean, &finish_error);
        const Status capture_status = capture.status();
        const deepwin_market_data::StreamHealth health = stream.health();
        const deepwin_market_data::StreamStats& stream_stats = stream.stats();

        Json output;
        output["ok"] = stream_ok && capture_ok && clean;
        output["datagrams"] = capture_status.datagrams;
        output["hardware_timestamps"] = capture_status.hardware_timestamps;
        output["missing_hardware_timestamps"] = capture_status.missing_hardware_timestamps;
        output["software_timestamp_fallbacks"] = capture_status.software_timestamp_fallbacks;
        output["idle_events"] = capture_status.idle_events;
        output["payload_bytes"] = capture_status.payload_bytes;
        output["accepted_events"] = capture_status.accepted_events;
        output["journal_events"] = capture_status.journal_events;
        output["journal_errors"] = capture_status.journal_errors;
        output["journal_overflows"] = capture_status.journal_overflows;
        output["last_receive_ns"] = capture_status.last_receive_ns;
        output["stats"]["receive_batches"] = stream_stats.receive_batches;
        output["stats"]["ingress_high_water"] = stream_stats.ingress_high_water;
        output["stats"]["kernel_drops"] = stream_stats.kernel_drops;
        output["stats"]["journal_queue_high_water"] = capture_status.queue_high_water;
        output["stats"]["journal_published_offset"] = capture_status.journal_published_offset;
        output["stats"]["journal_flushed_offset"] = capture_status.journal_flushed_offset;
        output["stats"]["journal_flush_count"] = capture_status.flush_count;
        output["stats"]["journal_clean"] = clean && capture_status.journal_closed;
        output["health"]["input_valid"] = health.input_valid;
        output["health"]["processing_valid"] = health.processing_valid && !capture_status.failed;
        output["health"]["recording_failed"] = health.recording_failed;
        output["health"]["recording_required"] = health.recording_required;
        output["health"]["journal_degraded"] = capture_status.journal_degraded;
        append_cpu_affinity(&output, capture_status.cpu_affinity);
        const std::string final_error = !stream_error.empty() ? stream_error : finish_error;
        if (!final_error.empty()) output["error"] = final_error;
        std::cout << output.dump() << "\n";
        return output["ok"].get<bool>() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "unknown Shanghai journal capture failure\n";
        return 1;
    }
}

}  // namespace sse_journal_capture

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: t0_sse_journal_capture CONFIG.json\n";
        return 1;
    }
    return sse_journal_capture::run_cli(argv[1]);
}
