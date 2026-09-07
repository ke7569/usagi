#ifndef SSE_EVENT_PIPELINE_H
#define SSE_EVENT_PIPELINE_H

#include "sse_event.h"
#include "sse_latency_histogram.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sse_pipeline {

struct PipelineOptions {
    std::size_t producers;
    std::size_t shards;
    std::size_t ingress_capacity;
    std::size_t shard_capacity;
    std::size_t journal_capacity;
    std::size_t pending_snapshot_capacity;
    bool replay;
    bool tick_sampling;
    bool live_clock;
    std::uint64_t latency_start_realtime_ns;
    std::uint64_t history_warmup_until_ns;
    PipelineOptions() : producers(1), shards(8), ingress_capacity(8192),
        shard_capacity(65536), journal_capacity(65536),
        pending_snapshot_capacity(262144), replay(false),
        tick_sampling(false), live_clock(true), latency_start_realtime_ns(0), history_warmup_until_ns(0) {}
};

struct PipelineStats {
    std::uint64_t submitted, ingress_overflow, events, duplicates, late_ticks;
    std::uint64_t journal_written, journal_overflow, journal_errors;
    std::uint64_t pending_snapshots, pending_overflow, routing_errors;
    std::vector<std::uint64_t> shard_consumed, shard_overflow, shard_errors;
    std::vector<std::size_t> ingress_high_water, shard_high_water;
    std::vector<std::vector<std::size_t> > channel_stock_counts;
    LatencyHistogram ingress_latency;
    std::vector<LatencyHistogram> shard_service_latency;
    std::vector<LatencyHistogram> shard_end_to_end_latency;
    std::vector<LatencyHistogram> shard_sample_end_to_end_latency;
    std::size_t journal_high_water;
    bool compute_healthy, journal_healthy;
};

typedef std::function<bool(const Event&)> EventConsumer;

class EventPipeline {
public:
    explicit EventPipeline(const PipelineOptions& options);
    ~EventPipeline();
    EventPipeline(const EventPipeline&) = delete;
    EventPipeline& operator=(const EventPipeline&) = delete;

    // cpu_ids: dispatcher, journal, shard0..N. Empty is reserved for tests.
    bool start(const std::vector<EventConsumer>& consumers,
               const EventConsumer& journal, const std::vector<int>& cpu_ids,
               std::string* error);
    // One submitting thread per producer. A full ingress returns immediately.
    // origin_ns=0 stamps the submission with the current monotonic clock.  A
    // non-zero origin is useful when a caller already captured the ingress
    // timestamp and wants queue/scheduling delay included in end-to-end stats.
    bool submit(std::size_t producer, const Event& event,
                std::uint64_t origin_ns = 0U);
    // Stop/join external producers before calling stop; drains accepted events.
    void stop();
    // Single-producer historical warmup only, while submissions are paused.
    void drain();
    // Final snapshot, after stop. No locks or statistics formatting on receive.
    PipelineStats stats() const;
    std::string error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
