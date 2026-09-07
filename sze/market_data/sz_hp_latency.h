#ifndef SZ_HP_LATENCY_H
#define SZ_HP_LATENCY_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sz_hp {

uint64_t latency_now_ns();

struct LatencySummary {
    uint64_t count;
    double mean_ns;
    uint64_t p50_ns;
    uint64_t p99_ns;
    uint64_t max_ns;

    LatencySummary();
};

class LatencyRecorder {
public:
    explicit LatencyRecorder(bool enabled = false);

    bool enabled() const;
    void set_enabled(bool enabled);
    void add(uint64_t elapsed_ns);
    void clear();
    LatencySummary summarize() const;
    const std::vector<uint64_t>& samples() const;

private:
    bool enabled_;
    std::vector<uint64_t> samples_;
};

struct LatencyReport {
    LatencySummary order;
    LatencySummary trade;
    LatencySummary sample_factor;
    LatencySummary sample_end_to_end;
    uint64_t timer_overhead_p50_ns;
    uint64_t timer_overhead_max_ns;

    LatencyReport();
    void clear();
};

LatencyReport benchmark_timer_overhead(size_t iterations);

}  // namespace sz_hp

#endif  // SZ_HP_LATENCY_H
