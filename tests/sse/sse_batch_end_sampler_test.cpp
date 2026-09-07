#include "sse/sampling/sse_batch_end_sampler.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using sse_live_sampling::BatchEnd;
using sse_live_sampling::BatchEndSampler;
using sse_live_sampling::Candidate;
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

void advance(BatchEndSampler* sampler, std::uint64_t time,
             std::vector<BatchEnd>* closed) {
    std::string error;
    closed->clear();
    require(sampler->advance_to_event(time, closed, &error),
            error.empty() ? "advance_to_event failed" : error.c_str());
}

void commit(BatchEndSampler* sampler, const char* instrument,
            std::uint64_t time, std::uint64_t cut_index,
            std::vector<BatchEnd>* closed) {
    advance(sampler, time, closed);
    Candidate value = candidate(instrument, cut_index, time);
    std::string error;
    require(sampler->commit_applied_event(instrument, &value, true, &error),
            error.empty() ? "commit_applied_event failed" : error.c_str());
}

void timer(BatchEndSampler* sampler, std::uint64_t time,
           std::vector<BatchEnd>* closed) {
    std::string error;
    closed->clear();
    require(sampler->on_timer(time, closed, &error),
            error.empty() ? "on_timer failed" : error.c_str());
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

void test_quiet_stock_closes_while_other_stock_continues() {
    std::vector<std::string> instruments;
    instruments.push_back("A");
    instruments.push_back("B");
    BatchEndSampler sampler(instruments);
    std::vector<BatchEnd> closed;
    commit(&sampler, "A", 1000000ULL, 1ULL, &closed);
    commit(&sampler, "B", 1050000ULL, 2ULL, &closed);
    timer(&sampler, 1100001ULL, &closed);
    require(closed.size() == 1U && closed[0].candidates.size() == 1U &&
                closed[0].candidates[0].instrument_id == "A",
            "quiet A did not close at its own deadline while B continued");
    commit(&sampler, "B", 1150000ULL, 3ULL, &closed);
    timer(&sampler, 1250001ULL, &closed);
    require(closed.size() == 1U && closed[0].candidates.size() == 1U &&
                closed[0].candidates[0].instrument_id == "B",
            "B deadline was not independent of A");
}

void test_exact_threshold_and_per_stock_rearm() {
    std::vector<std::string> instruments;
    instruments.push_back("A");
    instruments.push_back("B");
    BatchEndSampler sampler(instruments);
    std::vector<BatchEnd> closed;
    commit(&sampler, "A", 1000000ULL, 1ULL, &closed);
    commit(&sampler, "B", 1000000ULL, 2ULL, &closed);
    timer(&sampler, 1100000ULL, &closed);
    require(closed.empty(), "exactly 100us incorrectly closed a stock");
    timer(&sampler, 1100001ULL, &closed);
    require(closed.size() == 2U && closed[0].candidates.size() == 1U &&
                closed[1].candidates.size() == 1U &&
                closed[0].candidates[0].instrument_id == "A" &&
                closed[1].candidates[0].instrument_id == "B",
            "simultaneous deadlines were not deterministic per stock");

    commit(&sampler, "A", 2000000ULL, 3ULL, &closed);
    commit(&sampler, "B", 2000000ULL, 5ULL, &closed);
    commit(&sampler, "A", 2090000ULL, 4ULL, &closed);
    timer(&sampler, 2100001ULL, &closed);
    require(closed.size() == 1U && closed[0].candidates.size() == 1U &&
                closed[0].candidates[0].instrument_id == "B",
            "repeated A update incorrectly postponed or closed B");
    timer(&sampler, 2190001ULL, &closed);
    require(closed.size() == 1U && closed[0].candidates.size() == 1U &&
                closed[0].candidates[0].cut_index == 4ULL,
            "rearmed A batch did not retain its latest candidate");
}

void test_null_candidate_only_rearms_pending_stock() {
    BatchEndSampler sampler(std::vector<std::string>(1, "A"));
    std::vector<BatchEnd> closed;
    advance(&sampler, 1000ULL, &closed);
    std::string error;
    require(sampler.commit_applied_event("A", 0, true, &error), error.c_str());
    timer(&sampler, 101001ULL, &closed);
    require(closed.empty(), "activity-only event opened an empty sample batch");

    commit(&sampler, "A", 102000ULL, 1ULL, &closed);
    advance(&sampler, 103000ULL, &closed);
    require(sampler.commit_applied_event("A", 0, true, &error), error.c_str());
    timer(&sampler, 203002ULL, &closed);
    require(closed.size() == 1U && closed[0].candidates.size() == 1U,
            "null candidate did not preserve pending candidate batch");
}

void test_gate_initialization_and_triggers() {
    TickSampleGate gate;
    SampleDecision decision;
    std::string error;
    const double threshold = 500.0;

    require(gate.observe_batch_end(cut(1, 34200000000ULL, 10.0, 1000.0, 1000),
                                   threshold, &decision, &error), error.c_str());
    require(!decision.accepted && gate.initialized(),
            "first valid two-sided cut should initialize without sampling");
    require(gate.observe_batch_end(cut(2, 34200000000ULL, 10.0, 1300.0, 1100),
                                   threshold, &decision, &error), error.c_str());
    require(!decision.accepted,
            "same exchange timestamp emitted a second sample");
    require(gate.observe_batch_end(cut(3, 34200001000ULL, 10.0, 1500.0, 1100),
                                   threshold, &decision, &error), error.c_str());
    require(decision.accepted &&
                (decision.reasons & sse_live_sampling::kTurnoverSampleReason) != 0,
            "amount trigger was not accepted");

    require(gate.observe_batch_end(cut(4, 34200001000ULL, 10.0, 1700.0, 1200),
                                   threshold, &decision, &error), error.c_str());
    require(!decision.accepted,
            "duplicate exchange timestamp after a sample emitted again");
    require(gate.observe_batch_end(cut(5, 34200002000ULL, 10.0, 1700.0, 1200),
                                   threshold, &decision, &error), error.c_str());
    require(!decision.accepted,
            "duplicate-time cut changed the next exchange-time window");

    gate.reset();
    require(gate.observe_batch_end(cut(1, 34200000000ULL, 10.0, 1000.0, 1000),
                                   threshold, &decision, &error), error.c_str());
    require(gate.observe_batch_end(cut(2, 34300000000ULL, 10.0, 1000.0, 1000),
                                   threshold, &decision, &error), error.c_str());
    require(decision.accepted &&
                (decision.reasons & sse_live_sampling::kTimeSampleReason) != 0,
            "exact 100-second exchange-time trigger was not accepted");

    gate.reset();
    require(gate.observe_batch_end(cut(1, 34200000000ULL, 10.0, 1000.0, 1000),
                                   threshold, &decision, &error), error.c_str());
    require(gate.observe_batch_end(cut(2, 34200001000ULL, 10.01, 1000.0, 1099),
                                   threshold, &decision, &error), error.c_str());
    require(!decision.accepted, "99-share price change triggered a sample");
    require(gate.observe_batch_end(cut(3, 34200002000ULL, 10.01, 1000.0, 1100),
                                   threshold, &decision, &error), error.c_str());
    require(decision.accepted &&
                (decision.reasons & sse_live_sampling::kChangeSampleReason) != 0,
            "100-share price change did not trigger a sample");
}

void test_gate_waits_for_first_continuous_window() {
    TickSampleGate gate;
    SampleDecision decision;
    std::string error;
    require(gate.observe_batch_end(cut(1, 34199999999ULL, 10.0, 1000.0, 1000,
                                       false, true),
                                   500.0, &decision, &error), error.c_str());
    require(!gate.initialized() && !decision.accepted,
            "pre-open cut initialized the sample window");
    require(gate.observe_batch_end(cut(2, 34200000000ULL, 10.0, 1000.0, 1000),
                                   500.0, &decision, &error), error.c_str());
    require(gate.initialized() && !decision.accepted,
            "first continuous cut was not a no-sample initialization");
}

void test_repeated_cut_cannot_acquire_a_new_exchange_time() {
    TickSampleGate gate;
    SampleDecision decision;
    std::string error;
    require(gate.observe_batch_end(cut(1, 34200000000ULL, 10.0, 0.0, 0),
                                   500.0, &decision, &error), error.c_str());
    require(!gate.observe_batch_end(cut(1, 34300000000ULL, 10.0, 500.0, 100),
                                    500.0, &decision, &error),
            "same book cut with a different exchange timestamp was accepted");
}

void test_invalid_sequence_is_sticky() {
    BatchEndSampler sampler(std::vector<std::string>(1, "A"));
    std::vector<BatchEnd> closed;
    commit(&sampler, "A", 1000ULL, 1ULL, &closed);
    std::string error;
    advance(&sampler, 1001ULL, &closed);
    require(!sampler.commit_applied_event("A", 0, false, &error) &&
                !sampler.healthy(),
            "unhealthy commit was not sticky");
    require(!sampler.on_timer(200000ULL, &closed, &error) &&
                error.find("unhealthy") != std::string::npos,
            "unhealthy sampler accepted a later timer");
}

void test_bounded_heap_stress() {
    std::vector<std::string> instruments;
    instruments.push_back("A");
    instruments.push_back("B");
    instruments.push_back("C");
    BatchEndSampler sampler(instruments);
    std::vector<BatchEnd> closed;
    const std::uint64_t start = 1000000ULL;
    const std::size_t updates = 10000U;
    for (std::size_t i = 0; i < updates; ++i) {
        const std::uint64_t now = start + static_cast<std::uint64_t>(i) * 10ULL;
        commit(&sampler, "A", now, static_cast<std::uint64_t>(i + 1U), &closed);
        require(closed.empty(), "sub-threshold stress update closed A");
        require(sampler.pending_instruments() <= instruments.size(),
                "sampler heap exceeded configured instrument count");
    }
    const std::uint64_t last = start + static_cast<std::uint64_t>(updates - 1U) * 10ULL;
    require(sampler.pending_instruments() == 1U,
            "stress run did not retain exactly one pending A instrument");
    require(sampler.next_deadline_ns() == last + 100001ULL,
            "stress run deadline does not match A's latest update");
    timer(&sampler, last + 100001ULL, &closed);
    require(closed.size() == 1U && closed[0].candidates.size() == 1U &&
                closed[0].candidates[0].cut_index == updates,
            "stress run did not emit A's latest candidate exactly once");
}

void test_recovery_discards_old_pending_and_allows_reuse() {
    BatchEndSampler sampler(std::vector<std::string>(1, "A"));
    std::vector<BatchEnd> closed;
    commit(&sampler, "A", 1000ULL, 1ULL, &closed);
    sampler.mark_sequence_gap();
    require(!sampler.healthy() && sampler.pending_instruments() == 0U,
            "sequence gap retained a pending instrument");
    sampler.recover();
    commit(&sampler, "A", 2000ULL, 1ULL, &closed);
    timer(&sampler, 102001ULL, &closed);
    require(closed.size() == 1U && closed[0].candidates.size() == 1U &&
                closed[0].candidates[0].cut_index == 1ULL,
            "recovery did not permit a clean cut-index reuse");
}

}  // namespace

int main() {
    test_quiet_stock_closes_while_other_stock_continues();
    test_exact_threshold_and_per_stock_rearm();
    test_null_candidate_only_rearms_pending_stock();
    test_gate_initialization_and_triggers();
    test_gate_waits_for_first_continuous_window();
    test_repeated_cut_cannot_acquire_a_new_exchange_time();
    test_invalid_sequence_is_sticky();
    test_bounded_heap_stress();
    test_recovery_discards_old_pending_and_allows_reuse();
    std::cout << "sse_live_sampling_test: PASS" << std::endl;
    return 0;
}
