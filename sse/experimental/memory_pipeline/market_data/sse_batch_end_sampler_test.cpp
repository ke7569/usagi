#include "sse_batch_end_sampler.h"

#include <poll.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using sse_live_sampling::BatchEnd;
using sse_live_sampling::BatchEndSampler;
using sse_live_sampling::Candidate;
using sse_live_sampling::MonotonicOneShotTimer;
using sse_live_sampling::SampleDecision;
using sse_live_sampling::TickCut;
using sse_live_sampling::TickSampleGate;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "sse_live_sampling_test: " << message << std::endl;
        std::exit(1);
    }
}

Candidate candidate(const char* instrument, std::uint64_t cut,
                    std::uint64_t receive_ns,
                    std::uint64_t exchange_micros = 34200000000ULL) {
    Candidate value;
    value.instrument_id = instrument;
    value.cut_index = cut;
    value.source_sequence = cut + 1000ULL;
    value.exchange_time_of_day_micros = exchange_micros;
    value.local_receive_ns = receive_ns;
    return value;
}

void commit(BatchEndSampler* sampler, std::uint64_t receive_ns,
            const Candidate* value, BatchEnd* closed, bool* did_close) {
    std::string error;
    require(sampler->advance_to_event(receive_ns, closed, did_close, &error),
            error.c_str());
    require(sampler->commit_applied_event(value, true, &error), error.c_str());
}

TickCut cut(std::uint64_t index, std::uint64_t exchange_micros,
            double mid, double turnover, std::int64_t volume,
            bool continuous = true, bool valid = true) {
    TickCut value;
    value.cut_index = index;
    value.exchange_time_of_day_micros = exchange_micros;
    value.mid_price = mid;
    value.cumulative_turnover = turnover;
    value.cumulative_volume = volume;
    value.continuous_trading = continuous;
    value.valid_book = valid;
    return value;
}

void test_strict_threshold_and_rearm() {
    BatchEndSampler sampler;
    BatchEnd closed;
    bool did_close = false;
    const std::uint64_t t0 = 1000000000ULL;
    Candidate first = candidate("600000", 1ULL, t0);
    commit(&sampler, t0, &first, &closed, &did_close);
    require(!did_close, "first event unexpectedly closed a batch");
    require(sampler.next_deadline_ns() == t0 + 100001ULL,
            "strict deadline is not threshold plus one nanosecond");

    std::string error;
    require(sampler.on_timer(t0 + 100000ULL, &closed, &did_close, &error),
            error.c_str());
    require(!did_close, "gap exactly 100us closed the batch");

    Candidate second = candidate("600000", 2ULL, t0 + 100000ULL);
    commit(&sampler, second.local_receive_ns, &second, &closed, &did_close);
    require(!did_close, "event at exact threshold closed the batch");
    require(sampler.next_deadline_ns() == t0 + 200001ULL,
            "event inside batch did not rearm deadline");

    require(sampler.on_timer(t0 + 200001ULL, &closed, &did_close, &error),
            error.c_str());
    require(did_close, "gap greater than 100us did not close the batch");
    require(closed.candidates.size() == 1U &&
                closed.candidates[0].cut_index == 2ULL,
            "batch did not coalesce to final per-instrument candidate");
    require(closed.inactivity_ns == 100001ULL,
            "batch close did not retain inactivity duration");
}

void test_late_timer_and_empty_batch() {
    BatchEndSampler sampler;
    BatchEnd closed;
    bool did_close = false;
    Candidate first = candidate("600001", 10ULL, 1000ULL);
    commit(&sampler, 1000ULL, &first, &closed, &did_close);

    std::string error;
    require(sampler.advance_to_event(101001ULL, &closed, &did_close, &error),
            error.c_str());
    require(did_close, "next-event gap did not close late timer batch");
    require(closed.reason == sse_live_sampling::kBatchClosedByNextEvent &&
                closed.candidates.size() == 1U &&
                closed.candidates[0].cut_index == 10ULL,
            "late timer close returned the wrong old batch");

    // The new event is non-candidate activity and belongs to the new batch.
    require(sampler.commit_applied_event(0, true, &error), error.c_str());
    require(sampler.on_timer(201002ULL, &closed, &did_close, &error), error.c_str());
    require(did_close && closed.candidates.empty(),
            "activity-only batch created a model candidate");
}

void test_multi_instrument_and_duplicate_suppression() {
    BatchEndSampler sampler;
    BatchEnd closed;
    bool did_close = false;
    Candidate a1 = candidate("600000", 5ULL, 1000ULL);
    Candidate b1 = candidate("600001", 7ULL, 1010ULL);
    Candidate a2 = candidate("600000", 6ULL, 1020ULL);
    commit(&sampler, 1000ULL, &a1, &closed, &did_close);
    commit(&sampler, 1010ULL, &b1, &closed, &did_close);
    commit(&sampler, 1020ULL, &a2, &closed, &did_close);
    std::string error;
    require(sampler.on_timer(101021ULL, &closed, &did_close, &error), error.c_str());
    require(did_close && closed.candidates.size() == 2U,
            "batch did not emit one candidate per dirty instrument");
    require(closed.candidates[0].instrument_id == "600000" &&
                closed.candidates[0].cut_index == 6ULL &&
                closed.candidates[1].instrument_id == "600001",
            "batch candidate ordering/coalescing is not deterministic");

    Candidate duplicate = candidate("600000", 6ULL, 200000ULL);
    commit(&sampler, duplicate.local_receive_ns, &duplicate, &closed, &did_close);
    require(sampler.on_timer(300001ULL, &closed, &did_close, &error), error.c_str());
    require(did_close && closed.candidates.empty(),
            "already-emitted cut was emitted twice");
    require(sampler.stats().duplicate_candidates == 1ULL,
            "duplicate candidate metric was not updated");
}

void test_fail_closed_and_recovery() {
    BatchEndSampler sampler;
    BatchEnd closed;
    bool did_close = false;
    Candidate first = candidate("600000", 1ULL, 1000ULL);
    commit(&sampler, 1000ULL, &first, &closed, &did_close);
    sampler.mark_sequence_gap();
    std::string error;
    require(!sampler.on_timer(200000ULL, &closed, &did_close, &error) &&
                error.find("unhealthy") != std::string::npos,
            "sequence gap did not fail closed");
    sampler.recover();
    Candidate recovered = candidate("600000", 2ULL, 300000ULL);
    commit(&sampler, recovered.local_receive_ns, &recovered, &closed, &did_close);
    require(sampler.on_timer(400001ULL, &closed, &did_close, &error) &&
                did_close && closed.candidates.size() == 1U,
            "explicit recovery did not start a fresh batch");

    Candidate next = candidate("600000", 3ULL, 500000ULL);
    commit(&sampler, next.local_receive_ns, &next, &closed, &did_close);
    require(!sampler.advance_to_event(499999ULL, &closed, &did_close, &error) &&
                !sampler.healthy(),
            "non-monotonic receive time did not invalidate sampler");
}

void test_sample_gate() {
    TickSampleGate gate;
    SampleDecision decision;
    std::string error;
    require(gate.observe_batch_end(cut(1, 34200000000ULL, 10.0, 1000.0, 1000),
                                   500.0, &decision, &error), error.c_str());
    require(!decision.accepted && gate.initialized(),
            "first valid cut should initialize without sampling");
    require(gate.observe_batch_end(cut(2, 34201000000ULL, 10.0, 1200.0, 1050),
                                   500.0, &decision, &error), error.c_str());
    require(!decision.accepted, "cut without standard trigger was accepted");
    require(gate.observe_batch_end(cut(3, 34202000000ULL, 10.0, 1500.0, 1050),
                                   500.0, &decision, &error), error.c_str());
    require(decision.accepted &&
                (decision.reasons & sse_live_sampling::kTurnoverSampleReason) != 0,
            "turnover trigger was not accepted");

    gate.reset();
    require(gate.observe_batch_end(cut(1, 34200000000ULL, 10.0, 1000.0, 1000),
                                   1000000.0, &decision, &error), error.c_str());
    require(gate.observe_batch_end(cut(2, 34300000000ULL, 10.0, 1000.0, 1000),
                                   1000000.0, &decision, &error), error.c_str());
    require(decision.accepted &&
                (decision.reasons & sse_live_sampling::kTimeSampleReason) != 0,
            "100-second exchange-time trigger was not accepted");

    gate.reset();
    require(gate.observe_batch_end(cut(1, 34200000000ULL, 10.0, 1000.0, 1000),
                                   1000000.0, &decision, &error), error.c_str());
    require(gate.observe_batch_end(cut(2, 34201000000ULL, 10.01, 1000.0, 1099),
                                   1000000.0, &decision, &error), error.c_str());
    require(!decision.accepted, "99-share price change triggered a sample");
    require(gate.observe_batch_end(cut(3, 34202000000ULL, 10.01, 1000.0, 1100),
                                   1000000.0, &decision, &error), error.c_str());
    require(decision.accepted &&
                (decision.reasons & sse_live_sampling::kChangeSampleReason) != 0,
            "100-share price change did not trigger a sample");

    gate.reset();
    require(gate.observe_batch_end(cut(1, 34200000000ULL, 10.0, 1000.0, 1000,
                                           false, true),
                                   500.0, &decision, &error), error.c_str());
    require(!gate.initialized() && !decision.accepted,
            "non-continuous cut initialized the sample window");
    require(!gate.observe_batch_end(cut(1, 34200000000ULL, 10.0, 1000.0, 1000),
                                    0.0, &decision, &error),
            "zero turnover threshold did not fail closed");
}

void test_one_shot_timerfd() {
    MonotonicOneShotTimer timer;
    std::string error;
    require(timer.open(&error), error.c_str());
    std::uint64_t now = 0ULL;
    require(MonotonicOneShotTimer::now_ns(&now, &error), error.c_str());
    require(timer.arm_absolute(now + 1000000ULL, &error), error.c_str());
    pollfd descriptor;
    descriptor.fd = timer.fd();
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    require(::poll(&descriptor, 1, 250) == 1 && (descriptor.revents & POLLIN) != 0,
            "one-shot timerfd did not fire");
    std::uint64_t expirations = 0ULL;
    require(timer.read_expirations(&expirations, &error) && expirations == 1ULL,
            "one-shot timerfd expiration count is wrong");
    descriptor.revents = 0;
    require(::poll(&descriptor, 1, 0) == 0,
            "timerfd remained periodic after one-shot expiration");
    require(timer.disarm(&error), error.c_str());
}

}  // namespace

int main() {
    test_strict_threshold_and_rearm();
    test_late_timer_and_empty_batch();
    test_multi_instrument_and_duplicate_suppression();
    test_fail_closed_and_recovery();
    test_sample_gate();
    test_one_shot_timerfd();
    std::cout << "sse_live_sampling_test: PASS" << std::endl;
    return 0;
}
