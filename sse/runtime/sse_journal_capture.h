#ifndef USAGI_SSE_JOURNAL_CAPTURE_H
#define USAGI_SSE_JOURNAL_CAPTURE_H

#include "sse/runtime/sse_cpu_affinity.h"
#include "sse/runtime/sse_journal_transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sse_journal_capture {

struct Status {
    Status()
        : accepting(false), stop_requested(false), failed(false), journal_degraded(false),
          journal_closed(false), journal_ready(false),
          datagrams(0), hardware_timestamps(0), missing_hardware_timestamps(0),
          software_timestamp_fallbacks(0), idle_events(0), payload_bytes(0), last_receive_ns(0),
          accepted_events(0), journal_events(0), journal_errors(0), journal_overflows(0),
          queue_size(0), queue_high_water(0), latest_event_id(0), latest_feed_sequence(0),
          journal_published_offset(0), journal_flushed_offset(0), flush_count(0) {}

    bool accepting;
    bool stop_requested;
    bool failed;
    bool journal_degraded;
    bool journal_closed;
    bool journal_ready;
    std::uint64_t datagrams;
    std::uint64_t hardware_timestamps, missing_hardware_timestamps, software_timestamp_fallbacks;
    std::uint64_t idle_events;
    std::uint64_t payload_bytes;
    std::uint64_t last_receive_ns;
    std::uint64_t accepted_events;
    std::uint64_t journal_events;
    std::uint64_t journal_errors;
    std::uint64_t journal_overflows;
    std::uint64_t queue_size;
    std::uint64_t queue_high_water;
    std::uint64_t latest_event_id;
    std::uint64_t latest_feed_sequence;
    std::uint64_t journal_published_offset;
    std::uint64_t journal_flushed_offset;
    std::uint64_t flush_count;
    std::string error;
    std::vector<sse_cpu::Cpu> cpu_affinity;
};

class Capture {
public:
    explicit Capture(const sse_journal::Config& config);
    ~Capture();

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Opens a new journal/ring epoch and starts the journal worker. The caller
    // supplies MarketDataStream callbacks after this succeeds.
    bool open(std::string* error = 0);

    // Called from the serialized MarketDataStream callback. The raw payload is
    // copied into a preallocated slot before the callback returns.
    bool on_event(const deepwin_market_data::StreamEvent& event,
                  std::string* error = 0);

    // Marks the producer as stopping. Already queued stream callbacks may still
    // be accepted until finish() is called after MarketDataStream::run returns.
    void request_stop();

    // Joins the journal worker, closes the journal, marks the ring stopped, and
    // releases the three-domain CPU lease. The clean journal remains replayable;
    // the ring is always marked not-ready after producer shutdown.
    bool finish(bool clean_shutdown, std::string* error = 0);

    bool is_open() const { return open_.load(std::memory_order_acquire); }
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    bool should_stop() const { return stop_requested_.load(std::memory_order_acquire) || failed(); }

    Status status() const;
    std::string error() const;
    const std::vector<sse_cpu::Cpu>& cpu_affinity() const { return cpu_affinity_; }

private:
    class Queue;

    void journal_loop();
    void fail(const std::string& message, sze_recovery::InvalidReason reason,
              std::uint64_t event_id, std::uint64_t feed_sequence);
    void fail_locked(const std::string& message);
    void publish_storage_metrics();

    sse_journal::Config config_;
    std::unique_ptr<Queue> queue_;
    sze_recovery::JournalWriter writer_;
    sze_recovery::ShmEventRing ring_;
    sse_cpu::Lease lease_;
    std::vector<sse_cpu::Cpu> cpu_affinity_;
    std::thread journal_thread_;

    std::atomic<bool> open_;
    std::atomic<bool> accepting_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> finish_requested_;
    std::atomic<bool> failed_;
    std::atomic<bool> journal_degraded_;
    std::atomic<bool> journal_ready_;
    std::atomic<bool> journal_start_failed_;
    std::atomic<bool> clean_shutdown_requested_;
    std::atomic<bool> journal_closed_;
    std::atomic<int> failure_reason_;

    std::atomic<std::uint64_t> datagrams_;
    std::atomic<std::uint64_t> hardware_timestamps_, missing_hardware_timestamps_, software_timestamp_fallbacks_;
    std::atomic<std::uint64_t> idle_events_;
    std::atomic<std::uint64_t> payload_bytes_;
    std::atomic<std::uint64_t> last_receive_ns_;
    std::atomic<std::uint64_t> accepted_events_;
    std::atomic<std::uint64_t> journal_events_;
    std::atomic<std::uint64_t> journal_errors_;
    std::atomic<std::uint64_t> journal_overflows_;
    std::atomic<std::uint64_t> latest_event_id_;
    std::atomic<std::uint64_t> latest_feed_sequence_;
    std::atomic<std::uint64_t> journal_published_offset_;
    std::atomic<std::uint64_t> journal_flushed_offset_;
    std::atomic<std::uint64_t> flush_count_;

    mutable std::mutex error_mutex_;
    std::string error_;
};

// Entry point used by the standalone t0_sse_journal_capture executable.
int run_cli(const std::string& config_path);

}  // namespace sse_journal_capture

#endif  // USAGI_SSE_JOURNAL_CAPTURE_H
