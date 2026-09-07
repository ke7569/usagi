#ifndef SSE_T0_MARKET_DATA_BATCH_END_SAMPLER_H
#define SSE_T0_MARKET_DATA_BATCH_END_SAMPLER_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sse_live_sampling {

static const std::uint64_t kBatchGapNanoseconds = 100000ULL;
static const std::uint64_t kSampleTimeTriggerMicros = 100000000ULL;
static const std::int64_t kSampleChangeMinVolume = 100;

enum BatchCloseReason {
    kBatchNotClosed = 0,
    kBatchClosedByTimer = 1,
    kBatchClosedByNextEvent = 2
};

struct Candidate {
    std::string instrument_id;
    std::uint64_t cut_index;
    std::uint64_t source_sequence;
    std::uint64_t exchange_time_of_day_micros;
    std::uint64_t local_receive_ns;

    Candidate();
};

struct BatchEnd {
    std::uint64_t batch_id;
    std::uint64_t last_activity_ns;
    std::uint64_t emitted_ns;
    std::uint64_t inactivity_ns;
    BatchCloseReason reason;
    std::vector<Candidate> candidates;

    BatchEnd();
};

struct SamplerStats {
    std::uint64_t committed_events;
    std::uint64_t closed_batches;
    std::uint64_t emitted_candidates;
    std::uint64_t empty_batches;
    std::uint64_t duplicate_candidates;
    std::uint64_t stale_timer_observations;
    std::uint64_t health_failures;

    SamplerStats();
};

// The caller must invoke advance_to_event before mutating the reconstructed
// book, then commit_applied_event after the update is complete. This ordering
// lets a late timer close the old batch before a new batch changes book state.
class BatchEndSampler {
public:
    explicit BatchEndSampler(std::uint64_t threshold_ns = kBatchGapNanoseconds);

    bool advance_to_event(std::uint64_t local_receive_ns,
                          BatchEnd* closed_batch,
                          bool* batch_closed,
                          std::string* error = 0);

    // candidate is null for batch activity that is not a CompleteOrderBookSH
    // sampling candidate. All committed events rearm the inactivity deadline.
    bool commit_applied_event(const Candidate* candidate,
                              bool sequence_healthy,
                              std::string* error = 0);

    bool on_timer(std::uint64_t monotonic_now_ns,
                  BatchEnd* closed_batch,
                  bool* batch_closed,
                  std::string* error = 0);

    void mark_sequence_gap();
    void recover();
    void reset_trading_day();

    bool healthy() const { return healthy_; }
    bool event_open() const { return event_open_; }
    bool has_active_batch() const { return active_; }
    std::uint64_t threshold_ns() const { return threshold_ns_; }
    std::uint64_t next_deadline_ns() const;
    const SamplerStats& stats() const { return stats_; }

private:
    bool close_batch(std::uint64_t emitted_ns,
                     BatchCloseReason reason,
                     BatchEnd* output);
    void discard_pending_batch();
    static bool gap_strictly_greater(std::uint64_t newer,
                                     std::uint64_t older,
                                     std::uint64_t threshold);

    std::uint64_t threshold_ns_;
    bool healthy_;
    bool event_open_;
    bool active_;
    std::uint64_t open_event_receive_ns_;
    std::uint64_t last_activity_ns_;
    std::uint64_t current_batch_id_;
    std::uint64_t next_batch_id_;
    std::map<std::string, Candidate> pending_candidates_;
    std::map<std::string, std::uint64_t> last_emitted_cut_index_;
    SamplerStats stats_;
};

enum SampleReason {
    kNoSampleReason = 0,
    kTurnoverSampleReason = 1,
    kTimeSampleReason = 2,
    kChangeSampleReason = 4
};

struct TickCut {
    std::uint64_t cut_index;
    std::uint64_t exchange_time_of_day_micros;
    double mid_price;
    double cumulative_turnover;
    std::int64_t cumulative_volume;
    bool continuous_trading;
    bool valid_book;

    TickCut();
};

struct SampleDecision {
    bool accepted;
    int reasons;
    double window_turnover;
    std::int64_t window_volume;
    std::uint64_t window_exchange_time_micros;

    SampleDecision();
};

// Implements the frozen GetByTurnoverAndChange gate after the caller has
// already established CompleteOrderBookSH batch-end eligibility.
class TickSampleGate {
public:
    TickSampleGate();

    bool observe_batch_end(const TickCut& cut,
                           double turnover_threshold,
                           SampleDecision* decision,
                           std::string* error = 0);
    void reset();
    bool initialized() const { return initialized_; }
    const TickCut& window_start() const { return window_start_; }

private:
    bool initialized_;
    TickCut window_start_;
};

// Thin Linux adapter intended for poll/epoll integration. It never starts a
// periodic thread; every arm replaces the previous absolute deadline.
class MonotonicOneShotTimer {
public:
    MonotonicOneShotTimer();
    ~MonotonicOneShotTimer();

    MonotonicOneShotTimer(const MonotonicOneShotTimer&) = delete;
    MonotonicOneShotTimer& operator=(const MonotonicOneShotTimer&) = delete;

    bool open(std::string* error = 0);
    bool arm_absolute(std::uint64_t deadline_ns, std::string* error = 0);
    bool disarm(std::string* error = 0);
    bool read_expirations(std::uint64_t* expirations, std::string* error = 0);
    int fd() const { return fd_; }

    static bool now_ns(std::uint64_t* value, std::string* error = 0);

private:
    int fd_;
};

}  // namespace sse_live_sampling

#endif  // SSE_T0_MARKET_DATA_BATCH_END_SAMPLER_H
