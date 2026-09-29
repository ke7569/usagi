#include "sse_batch_end_sampler.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

namespace sse_live_sampling {
namespace {

bool reject(const std::string& message, std::string* error) {
    if (error != 0) *error = message;
    return false;
}

std::uint64_t saturating_deadline(std::uint64_t start,
                                  std::uint64_t threshold) {
    // The contract is strict greater-than, so the first eligible nanosecond is
    // threshold + 1. Saturate rather than wrapping a corrupt timestamp.
    const std::uint64_t extra = threshold == std::numeric_limits<std::uint64_t>::max()
                                    ? threshold
                                    : threshold + 1ULL;
    if (start > std::numeric_limits<std::uint64_t>::max() - extra) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return start + extra;
}

}  // namespace

Candidate::Candidate()
    : instrument_id(), cut_index(0ULL), source_sequence(0ULL),
      exchange_time_of_day_micros(0ULL), local_receive_ns(0ULL) {}

BatchEnd::BatchEnd()
    : batch_id(0ULL), last_activity_ns(0ULL), emitted_ns(0ULL),
      inactivity_ns(0ULL), reason(kBatchNotClosed), candidates() {}

SamplerStats::SamplerStats()
    : committed_events(0ULL), closed_batches(0ULL), emitted_candidates(0ULL),
      empty_batches(0ULL), duplicate_candidates(0ULL),
      stale_timer_observations(0ULL), health_failures(0ULL) {}

BatchEndSampler::BatchEndSampler(std::uint64_t threshold_ns)
    : threshold_ns_(threshold_ns), healthy_(true), event_open_(false),
      active_(false), open_event_receive_ns_(0ULL), last_activity_ns_(0ULL),
      current_batch_id_(0ULL), next_batch_id_(1ULL), pending_candidates_(),
      last_emitted_cut_index_(), stats_() {}

bool BatchEndSampler::gap_strictly_greater(std::uint64_t newer,
                                           std::uint64_t older,
                                           std::uint64_t threshold) {
    return newer >= older && newer - older > threshold;
}

bool BatchEndSampler::advance_to_event(std::uint64_t local_receive_ns,
                                       BatchEnd* closed_batch,
                                       bool* batch_closed,
                                       std::string* error) {
    if (error != 0) error->clear();
    if (closed_batch == 0 || batch_closed == 0) {
        return reject("SSE batch sampler requires output arguments", error);
    }
    *batch_closed = false;
    *closed_batch = BatchEnd();
    if (!healthy_) {
        return reject("SSE batch sampler is unhealthy; explicit recovery is required", error);
    }
    if (event_open_) {
        return reject("SSE batch sampler event was advanced twice without commit", error);
    }
    if (active_ && local_receive_ns < last_activity_ns_) {
        mark_sequence_gap();
        return reject("SSE batch sampler received a non-monotonic local timestamp", error);
    }
    if (active_ && gap_strictly_greater(local_receive_ns, last_activity_ns_, threshold_ns_)) {
        close_batch(local_receive_ns, kBatchClosedByNextEvent, closed_batch);
        *batch_closed = true;
    }
    open_event_receive_ns_ = local_receive_ns;
    event_open_ = true;
    return true;
}

bool BatchEndSampler::commit_applied_event(const Candidate* candidate,
                                           bool sequence_healthy,
                                           std::string* error) {
    if (error != 0) error->clear();
    if (!healthy_) {
        return reject("SSE batch sampler is unhealthy; explicit recovery is required", error);
    }
    if (!event_open_) {
        return reject("SSE batch sampler commit requires advance_to_event", error);
    }
    if (!sequence_healthy) {
        mark_sequence_gap();
        return reject("SSE feed sequence gap invalidated the pending batch", error);
    }
    if (!active_) {
        active_ = true;
        current_batch_id_ = next_batch_id_++;
    }
    last_activity_ns_ = open_event_receive_ns_;
    if (candidate != 0) {
        if (candidate->instrument_id.empty() || candidate->cut_index == 0ULL ||
            candidate->local_receive_ns != open_event_receive_ns_) {
            mark_sequence_gap();
            return reject("invalid CompleteOrderBookSH candidate metadata", error);
        }
        std::map<std::string, std::uint64_t>::const_iterator emitted =
            last_emitted_cut_index_.find(candidate->instrument_id);
        std::map<std::string, Candidate>::iterator pending =
            pending_candidates_.find(candidate->instrument_id);
        const bool older_than_emitted =
            emitted != last_emitted_cut_index_.end() &&
            candidate->cut_index <= emitted->second;
        const bool older_than_pending =
            pending != pending_candidates_.end() &&
            candidate->cut_index <= pending->second.cut_index;
        if (older_than_emitted || older_than_pending) {
            ++stats_.duplicate_candidates;
        } else {
            pending_candidates_[candidate->instrument_id] = *candidate;
        }
    }
    ++stats_.committed_events;
    event_open_ = false;
    open_event_receive_ns_ = 0ULL;
    return true;
}

bool BatchEndSampler::on_timer(std::uint64_t monotonic_now_ns,
                               BatchEnd* closed_batch,
                               bool* batch_closed,
                               std::string* error) {
    if (error != 0) error->clear();
    if (closed_batch == 0 || batch_closed == 0) {
        return reject("SSE batch sampler requires timer output arguments", error);
    }
    *closed_batch = BatchEnd();
    *batch_closed = false;
    if (!healthy_) {
        return reject("SSE batch sampler is unhealthy; explicit recovery is required", error);
    }
    if (event_open_) {
        return reject("SSE batch sampler timer fired during an uncommitted book update", error);
    }
    if (!active_ || !gap_strictly_greater(monotonic_now_ns, last_activity_ns_, threshold_ns_)) {
        ++stats_.stale_timer_observations;
        return true;
    }
    close_batch(monotonic_now_ns, kBatchClosedByTimer, closed_batch);
    *batch_closed = true;
    return true;
}

bool BatchEndSampler::close_batch(std::uint64_t emitted_ns,
                                  BatchCloseReason reason,
                                  BatchEnd* output) {
    output->batch_id = current_batch_id_;
    output->last_activity_ns = last_activity_ns_;
    output->emitted_ns = emitted_ns;
    output->inactivity_ns = emitted_ns >= last_activity_ns_
                                ? emitted_ns - last_activity_ns_
                                : 0ULL;
    output->reason = reason;
    output->candidates.reserve(pending_candidates_.size());
    for (std::map<std::string, Candidate>::const_iterator it = pending_candidates_.begin();
         it != pending_candidates_.end(); ++it) {
        output->candidates.push_back(it->second);
        last_emitted_cut_index_[it->first] = it->second.cut_index;
    }
    ++stats_.closed_batches;
    stats_.emitted_candidates += output->candidates.size();
    if (output->candidates.empty()) ++stats_.empty_batches;
    discard_pending_batch();
    return true;
}

void BatchEndSampler::discard_pending_batch() {
    active_ = false;
    event_open_ = false;
    open_event_receive_ns_ = 0ULL;
    last_activity_ns_ = 0ULL;
    current_batch_id_ = 0ULL;
    pending_candidates_.clear();
}

void BatchEndSampler::mark_sequence_gap() {
    discard_pending_batch();
    healthy_ = false;
    ++stats_.health_failures;
}

void BatchEndSampler::recover() {
    discard_pending_batch();
    healthy_ = true;
}

void BatchEndSampler::reset_trading_day() {
    discard_pending_batch();
    last_emitted_cut_index_.clear();
    healthy_ = true;
    next_batch_id_ = 1ULL;
    stats_ = SamplerStats();
}

std::uint64_t BatchEndSampler::next_deadline_ns() const {
    return active_ ? saturating_deadline(last_activity_ns_, threshold_ns_) : 0ULL;
}

TickCut::TickCut()
    : cut_index(0ULL), exchange_time_of_day_micros(0ULL), mid_price(0.0),
      cumulative_turnover(0.0), cumulative_volume(0),
      continuous_trading(false), valid_book(false) {}

SampleDecision::SampleDecision()
    : accepted(false), reasons(kNoSampleReason), window_turnover(0.0),
      window_volume(0), window_exchange_time_micros(0ULL) {}

TickSampleGate::TickSampleGate() : initialized_(false), window_start_() {}

bool TickSampleGate::observe_batch_end(const TickCut& cut,
                                       double turnover_threshold,
                                       SampleDecision* decision,
                                       std::string* error) {
    if (error != 0) error->clear();
    if (decision == 0) return reject("SSE tick sample gate requires output", error);
    *decision = SampleDecision();
    if (!std::isfinite(cut.mid_price) || !std::isfinite(cut.cumulative_turnover) ||
        !std::isfinite(turnover_threshold) || turnover_threshold <= 0.0 ||
        cut.cut_index == 0ULL) {
        return reject("invalid SSE tick sampling cut or turnover threshold", error);
    }
    if (!cut.valid_book || !cut.continuous_trading) return true;
    if (!initialized_) {
        window_start_ = cut;
        initialized_ = true;
        return true;
    }
    if (cut.cut_index <= window_start_.cut_index ||
        cut.exchange_time_of_day_micros < window_start_.exchange_time_of_day_micros ||
        cut.cumulative_turnover < window_start_.cumulative_turnover ||
        cut.cumulative_volume < window_start_.cumulative_volume) {
        return reject("non-monotonic SSE tick sampling cut", error);
    }

    decision->window_turnover = cut.cumulative_turnover - window_start_.cumulative_turnover;
    decision->window_volume = cut.cumulative_volume - window_start_.cumulative_volume;
    decision->window_exchange_time_micros =
        cut.exchange_time_of_day_micros - window_start_.exchange_time_of_day_micros;
    if (cut.exchange_time_of_day_micros == window_start_.exchange_time_of_day_micros) {
        return true;
    }
    if (decision->window_turnover >= turnover_threshold) {
        decision->reasons |= kTurnoverSampleReason;
    }
    if (decision->window_exchange_time_micros >= kSampleTimeTriggerMicros) {
        decision->reasons |= kTimeSampleReason;
    }
    if (std::fabs(cut.mid_price - window_start_.mid_price) > 1.0e-6 &&
        decision->window_volume >= kSampleChangeMinVolume) {
        decision->reasons |= kChangeSampleReason;
    }
    decision->accepted = decision->reasons != kNoSampleReason;
    if (decision->accepted) window_start_ = cut;
    return true;
}

void TickSampleGate::reset() {
    initialized_ = false;
    window_start_ = TickCut();
}

MonotonicOneShotTimer::MonotonicOneShotTimer() : fd_(-1) {}

MonotonicOneShotTimer::~MonotonicOneShotTimer() {
    if (fd_ >= 0) ::close(fd_);
}

bool MonotonicOneShotTimer::open(std::string* error) {
    if (error != 0) error->clear();
    if (fd_ >= 0) return true;
    fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd_ < 0) {
        return reject(std::string("timerfd_create failed: ") + std::strerror(errno), error);
    }
    return true;
}

bool MonotonicOneShotTimer::arm_absolute(std::uint64_t deadline_ns,
                                         std::string* error) {
    if (error != 0) error->clear();
    if (fd_ < 0 && !open(error)) return false;
    if (deadline_ns == 0ULL) return disarm(error);
    itimerspec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_sec = static_cast<time_t>(deadline_ns / 1000000000ULL);
    spec.it_value.tv_nsec = static_cast<long>(deadline_ns % 1000000000ULL);
    if (::timerfd_settime(fd_, TFD_TIMER_ABSTIME, &spec, 0) != 0) {
        return reject(std::string("timerfd_settime failed: ") + std::strerror(errno), error);
    }
    return true;
}

bool MonotonicOneShotTimer::disarm(std::string* error) {
    if (error != 0) error->clear();
    if (fd_ < 0) return true;
    itimerspec spec;
    std::memset(&spec, 0, sizeof(spec));
    if (::timerfd_settime(fd_, 0, &spec, 0) != 0) {
        return reject(std::string("timerfd disarm failed: ") + std::strerror(errno), error);
    }
    return true;
}

bool MonotonicOneShotTimer::read_expirations(std::uint64_t* expirations,
                                              std::string* error) {
    if (error != 0) error->clear();
    if (expirations == 0) return reject("timerfd read requires output", error);
    *expirations = 0ULL;
    if (fd_ < 0) return reject("timerfd is not open", error);
    const ssize_t size = ::read(fd_, expirations, sizeof(*expirations));
    if (size == static_cast<ssize_t>(sizeof(*expirations))) return true;
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    return reject(std::string("timerfd read failed: ") + std::strerror(errno), error);
}

bool MonotonicOneShotTimer::now_ns(std::uint64_t* value, std::string* error) {
    if (error != 0) error->clear();
    if (value == 0) return reject("monotonic clock requires output", error);
    timespec now;
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return reject(std::string("clock_gettime failed: ") + std::strerror(errno), error);
    }
    *value = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
             static_cast<std::uint64_t>(now.tv_nsec);
    return true;
}

}  // namespace sse_live_sampling
