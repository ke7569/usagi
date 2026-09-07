#include "sze/market_data/sz_hp_latency.h"

#include <algorithm>
#include <ctime>

namespace sz_hp {

uint64_t latency_now_ns() {
    timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

LatencySummary::LatencySummary()
    : count(0), mean_ns(0.0), p50_ns(0), p99_ns(0), max_ns(0) {
}

LatencyRecorder::LatencyRecorder(bool enabled)
    : enabled_(enabled), samples_() {
    if (enabled_) {
        samples_.reserve(16384);
    }
}

bool LatencyRecorder::enabled() const {
    return enabled_;
}

void LatencyRecorder::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (enabled_ && samples_.capacity() == 0) {
        samples_.reserve(16384);
    }
}

void LatencyRecorder::add(uint64_t elapsed_ns) {
    if (enabled_) {
        samples_.push_back(elapsed_ns);
    }
}

void LatencyRecorder::clear() {
    samples_.clear();
}

LatencySummary LatencyRecorder::summarize() const {
    LatencySummary result;
    result.count = samples_.size();
    if (samples_.empty()) {
        return result;
    }
    std::vector<uint64_t> ordered = samples_;
    std::sort(ordered.begin(), ordered.end());
    long double total = 0.0;
    for (size_t i = 0; i < ordered.size(); ++i) {
        total += static_cast<long double>(ordered[i]);
    }
    result.mean_ns = static_cast<double>(total / static_cast<long double>(ordered.size()));
    const size_t p50_index = (ordered.size() - 1) / 2;
    const size_t p99_index = (ordered.size() - 1) * 99 / 100;
    result.p50_ns = ordered[p50_index];
    result.p99_ns = ordered[p99_index];
    result.max_ns = ordered.back();
    return result;
}

const std::vector<uint64_t>& LatencyRecorder::samples() const {
    return samples_;
}

LatencyReport::LatencyReport()
    : order(), trade(), sample_factor(), sample_end_to_end(),
      timer_overhead_p50_ns(0), timer_overhead_max_ns(0) {
}

void LatencyReport::clear() {
    *this = LatencyReport();
}

LatencyReport benchmark_timer_overhead(size_t iterations) {
    LatencyReport report;
    if (iterations == 0) {
        return report;
    }
    LatencyRecorder recorder(true);
    for (size_t i = 0; i < iterations; ++i) {
        const uint64_t begin = latency_now_ns();
        const uint64_t end = latency_now_ns();
        recorder.add(end >= begin ? end - begin : 0);
    }
    const LatencySummary summary = recorder.summarize();
    report.timer_overhead_p50_ns = summary.p50_ns;
    report.timer_overhead_max_ns = summary.max_ns;
    return report;
}

}  // namespace sz_hp
