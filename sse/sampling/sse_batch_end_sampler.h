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

enum BatchCloseReason { kBatchNotClosed = 0, kBatchClosedByTimer = 1, kBatchClosedByNextEvent = 2 };
struct Candidate {
    std::string instrument_id;
    std::uint64_t cut_index, source_sequence, exchange_time_of_day_micros, local_receive_ns;
    Candidate();
};
struct BatchEnd {
    std::uint64_t batch_id, last_activity_ns, emitted_ns, inactivity_ns;
    std::uint32_t packet_count;
    BatchCloseReason reason;
    std::vector<Candidate> candidates;
    BatchEnd();
};
struct SamplerStats {
    std::uint64_t committed_events, closed_batches, emitted_candidates, empty_batches;
    std::uint64_t duplicate_candidates, stale_timer_observations, health_failures;
    SamplerStats();
};

// One indexed deadline per configured instrument. Advance recorded time before
// any book mutation, then commit activity only for the updated instrument.
class BatchEndSampler {
public:
    explicit BatchEndSampler(const std::vector<std::string>& instruments,
                             std::uint64_t threshold_ns = kBatchGapNanoseconds);
    BatchEndSampler(const BatchEndSampler&) = delete;
    BatchEndSampler& operator=(const BatchEndSampler&) = delete;
    bool advance_to_event(std::uint64_t now, std::vector<BatchEnd>* closed, std::string* error = 0);
    // Null candidates rearm an existing pending cut, never another stock.
    bool commit_applied_event(const std::string& instrument, const Candidate* candidate,
                              bool sequence_healthy, std::string* error = 0);
    bool on_timer(std::uint64_t now, std::vector<BatchEnd>* closed, std::string* error = 0);
    void mark_sequence_gap();
    void recover();
    void reset_trading_day();
    bool healthy() const { return healthy_; }
    bool has_active_batch() const { return !heap_.empty(); }
    std::size_t pending_instruments() const { return heap_.size(); }
    std::uint64_t threshold_ns() const { return threshold_ns_; }
    std::uint64_t next_deadline_ns() const;
    const SamplerStats& stats() const { return stats_; }
private:
    struct Instrument {
        Candidate candidate;
        std::uint64_t last_activity, last_emitted_cut, batch_id;
        std::size_t heap_index;
        bool active;
        Instrument();
    };
    bool advance(std::uint64_t now, BatchCloseReason reason,
                 std::vector<BatchEnd>* closed, std::string* error);
    bool earlier(const Instrument* a, const Instrument* b) const;
    void swap_heap(std::size_t a, std::size_t b);
    void sift_up(std::size_t index);
    void sift_down(std::size_t index);
    void discard_pending();
    std::uint64_t threshold_ns_;
    bool healthy_, have_time_;
    std::uint64_t now_ns_, next_batch_id_;
    std::map<std::string, Instrument> instruments_;
    std::vector<Instrument*> heap_;
    SamplerStats stats_;
};

enum SampleReason { kNoSampleReason = 0, kTurnoverSampleReason = 1, kTimeSampleReason = 2, kChangeSampleReason = 4 };
struct TickCut {
    std::uint64_t cut_index, exchange_time_of_day_micros;
    double mid_price, cumulative_turnover;
    std::int64_t cumulative_volume;
    bool continuous_trading, valid_book;
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
// Per-instrument gate: initialization emits nothing, only accepted samples
// advance the window and exchange-time watermark.
class TickSampleGate {
public:
    TickSampleGate();
    bool observe_batch_end(const TickCut& cut, double turnover_threshold,
                           SampleDecision* decision, std::string* error = 0);
    void reset();
    bool initialized() const { return initialized_; }
    const TickCut& window_start() const { return window_start_; }
private:
    bool initialized_;
    TickCut window_start_;
};
}  // namespace sse_live_sampling
#endif
