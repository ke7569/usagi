#include "sse/sampling/sse_batch_end_sampler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sse_live_sampling {
namespace {
bool reject(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}
std::uint64_t deadline(std::uint64_t start, std::uint64_t threshold) {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    return start >= maximum - threshold ? maximum : start + threshold + 1;
}
}

Candidate::Candidate()
    : cut_index(0), source_sequence(0), exchange_time_of_day_micros(0), local_receive_ns(0) {}
BatchEnd::BatchEnd()
    : batch_id(0), last_activity_ns(0), emitted_ns(0), inactivity_ns(0), packet_count(0),
      reason(kBatchNotClosed) {}
SamplerStats::SamplerStats()
    : committed_events(0), closed_batches(0), emitted_candidates(0), empty_batches(0),
      duplicate_candidates(0), stale_timer_observations(0), health_failures(0) {}
BatchEndSampler::Instrument::Instrument()
    : last_activity(0), last_emitted_cut(0), batch_id(0), heap_index(0), active(false) {}

BatchEndSampler::BatchEndSampler(const std::vector<std::string>& instruments, std::uint64_t threshold_ns)
    : threshold_ns_(threshold_ns), healthy_(true), have_time_(false), now_ns_(0), next_batch_id_(1) {
    if (instruments.empty() || threshold_ns == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("SSE sampler requires instruments and a finite deadline");
    heap_.reserve(instruments.size());
    for (const std::string& id : instruments) {
        if (id.empty() || !instruments_.insert(std::make_pair(id, Instrument())).second)
            throw std::invalid_argument("duplicate or empty SSE sampler instrument");
        instruments_[id].candidate.instrument_id = id;
    }
}

bool BatchEndSampler::earlier(const Instrument* a, const Instrument* b) const {
    if (a->last_activity != b->last_activity) return a->last_activity < b->last_activity;
    return a->candidate.instrument_id < b->candidate.instrument_id;
}
void BatchEndSampler::swap_heap(std::size_t a, std::size_t b) {
    std::swap(heap_[a], heap_[b]);
    heap_[a]->heap_index = a;
    heap_[b]->heap_index = b;
}
void BatchEndSampler::sift_up(std::size_t index) {
    while (index && earlier(heap_[index], heap_[(index - 1) / 2])) {
        const std::size_t parent = (index - 1) / 2;
        swap_heap(index, parent);
        index = parent;
    }
}
void BatchEndSampler::sift_down(std::size_t index) {
    for (;;) {
        std::size_t child = index * 2 + 1;
        if (child >= heap_.size()) return;
        if (child + 1 < heap_.size() && earlier(heap_[child + 1], heap_[child])) ++child;
        if (!earlier(heap_[child], heap_[index])) return;
        swap_heap(child, index);
        index = child;
    }
}

bool BatchEndSampler::advance(std::uint64_t now, BatchCloseReason reason,
                              std::vector<BatchEnd>* closed, std::string* error) {
    if (error) error->clear();
    if (!closed) return reject("SSE sampler requires batch output", error);
    closed->clear();
    if (!healthy_) return reject("SSE sampler is unhealthy; explicit recovery required", error);
    if (have_time_ && now < now_ns_) {
        mark_sequence_gap();
        return reject("SSE sampler received non-monotonic time", error);
    }
    have_time_ = true;
    now_ns_ = now;
    while (!heap_.empty() && now >= heap_[0]->last_activity &&
           now - heap_[0]->last_activity > threshold_ns_) {
        Instrument* state = heap_[0];
        BatchEnd batch;
        batch.batch_id = state->batch_id;
        batch.last_activity_ns = state->last_activity;
        batch.emitted_ns = now;
        batch.inactivity_ns = now - state->last_activity;
        batch.reason = reason;
        batch.candidates.push_back(state->candidate);
        closed->push_back(batch);
        state->last_emitted_cut = state->candidate.cut_index;
        state->active = false;
        swap_heap(0, heap_.size() - 1);
        heap_.pop_back();
        if (!heap_.empty()) sift_down(0);
        ++stats_.closed_batches;
        ++stats_.emitted_candidates;
    }
    if (closed->empty() && reason == kBatchClosedByTimer) ++stats_.stale_timer_observations;
    return true;
}
bool BatchEndSampler::advance_to_event(std::uint64_t now, std::vector<BatchEnd>* closed, std::string* error) {
    return advance(now, kBatchClosedByNextEvent, closed, error);
}
bool BatchEndSampler::on_timer(std::uint64_t now, std::vector<BatchEnd>* closed, std::string* error) {
    return advance(now, kBatchClosedByTimer, closed, error);
}

bool BatchEndSampler::commit_applied_event(const std::string& id, const Candidate* candidate,
                                          bool sequence_healthy, std::string* error) {
    if (error) error->clear();
    if (!sequence_healthy) mark_sequence_gap();
    if (!healthy_ || !have_time_) return reject("SSE sampler commit requires healthy observed time", error);
    auto found = instruments_.find(id);
    if (found == instruments_.end()) return reject("unconfigured SSE sampler instrument", error);
    Instrument& state = found->second;
    if (candidate) {
        if (candidate->instrument_id != id || !candidate->cut_index || candidate->local_receive_ns != now_ns_) {
            mark_sequence_gap();
            return reject("invalid SSE candidate metadata", error);
        }
        if (candidate->cut_index <= state.last_emitted_cut ||
            (state.active && candidate->cut_index <= state.candidate.cut_index)) {
            ++stats_.duplicate_candidates;
            return true;
        }
        state.candidate = *candidate;
    } else if (!state.active) {
        ++stats_.committed_events;
        return true;
    }
    state.last_activity = now_ns_;
    if (state.active) {
        // One heap entry is updated in place, even for a very busy stock.
        sift_down(state.heap_index);
    } else {
        state.active = true;
        state.batch_id = next_batch_id_++;
        state.heap_index = heap_.size();
        heap_.push_back(&state);
        sift_up(state.heap_index);
    }
    ++stats_.committed_events;
    return true;
}
void BatchEndSampler::discard_pending() {
    for (Instrument* state : heap_) state->active = false;
    heap_.clear();
}
void BatchEndSampler::mark_sequence_gap() {
    discard_pending();
    healthy_ = false;
    ++stats_.health_failures;
}
void BatchEndSampler::recover() {
    discard_pending();
    healthy_ = true;
}
void BatchEndSampler::reset_trading_day() {
    discard_pending();
    for (auto& entry : instruments_) entry.second.last_emitted_cut = 0;
    healthy_ = true;
    have_time_ = false;
    now_ns_ = 0;
    next_batch_id_ = 1;
    stats_ = SamplerStats();
}
std::uint64_t BatchEndSampler::next_deadline_ns() const {
    return heap_.empty() ? 0 : deadline(heap_[0]->last_activity, threshold_ns_);
}

TickCut::TickCut()
    : cut_index(0), exchange_time_of_day_micros(0), mid_price(0), cumulative_turnover(0),
      cumulative_volume(0), continuous_trading(false), valid_book(false) {}
SampleDecision::SampleDecision()
    : accepted(false), reasons(kNoSampleReason), window_turnover(0), window_volume(0), window_exchange_time_micros(0) {}
TickSampleGate::TickSampleGate() : initialized_(false) {}

bool TickSampleGate::observe_batch_end(const TickCut& cut, double turnover_threshold,
                                      SampleDecision* decision, std::string* error) {
    if (error) error->clear();
    if (!decision) return reject("SSE tick sample gate requires output", error);
    *decision = SampleDecision();
    if (!std::isfinite(cut.mid_price) || !std::isfinite(cut.cumulative_turnover) ||
        cut.cumulative_turnover < 0 || cut.cumulative_volume < 0 ||
        !std::isfinite(turnover_threshold) || turnover_threshold <= 0 || !cut.cut_index)
        return reject("invalid SSE sampling cut or turnover threshold", error);
    if (!cut.valid_book || !cut.continuous_trading) return true;
    if (!initialized_) {
        window_start_ = cut;
        initialized_ = true;
        return true;
    }
    if (cut.cut_index < window_start_.cut_index ||
        (cut.cut_index == window_start_.cut_index &&
         cut.exchange_time_of_day_micros != window_start_.exchange_time_of_day_micros) ||
        cut.exchange_time_of_day_micros < window_start_.exchange_time_of_day_micros ||
        cut.cumulative_turnover < window_start_.cumulative_turnover ||
        cut.cumulative_volume < window_start_.cumulative_volume)
        return reject("non-monotonic SSE sampling cut", error);
    decision->window_turnover = cut.cumulative_turnover - window_start_.cumulative_turnover;
    decision->window_volume = cut.cumulative_volume - window_start_.cumulative_volume;
    decision->window_exchange_time_micros = cut.exchange_time_of_day_micros - window_start_.exchange_time_of_day_micros;
    if (cut.exchange_time_of_day_micros == window_start_.exchange_time_of_day_micros) return true;
    if (decision->window_turnover >= turnover_threshold) decision->reasons |= kTurnoverSampleReason;
    if (decision->window_exchange_time_micros >= kSampleTimeTriggerMicros) decision->reasons |= kTimeSampleReason;
    if (std::fabs(cut.mid_price - window_start_.mid_price) > 1e-6 && decision->window_volume >= kSampleChangeMinVolume)
        decision->reasons |= kChangeSampleReason;
    decision->accepted = decision->reasons != kNoSampleReason;
    if (decision->accepted) window_start_ = cut;
    return true;
}
void TickSampleGate::reset() {
    initialized_ = false;
    window_start_ = TickCut();
}
}  // namespace sse_live_sampling
