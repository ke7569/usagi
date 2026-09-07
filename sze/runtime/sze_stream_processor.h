#ifndef T0_PREDICTOR_SZE_STREAM_PROCESSOR_H
#define T0_PREDICTOR_SZE_STREAM_PROCESSOR_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/model/mix153060/mix153060_model.h"
#include "sze/sampling/mix153060_runtime.h"

#include "common/stream/MarketDataStream.h"

namespace sze_recovery {
struct CanonicalEvent;
}

namespace sze_stream {

enum class RecoveryTimeBasis {
    kSameBootMonotonic,
    kAnalysisExchangeTime
};

struct RecoveryTimeContext {
    RecoveryTimeBasis basis;
    std::uint64_t reference_mono_ns;
    std::uint64_t reference_realtime_ns;

    RecoveryTimeContext();
    static RecoveryTimeContext same_boot(std::uint64_t reference_mono_ns,
                                         std::uint64_t reference_realtime_ns);
    static RecoveryTimeContext analysis_exchange_time();
};

struct ProcessedSample {
    mix153060::Sample sample;
    float prediction;
    bool prediction_valid;
    std::uint64_t ingress_sequence;
    std::uint32_t channel_id;
    std::size_t record_offset;
    std::size_t record_size;
    std::uint64_t recovery_event_id;
    std::uint64_t recovery_feed_sequence;
    std::uint64_t recovery_channel_sequence;
    std::uint16_t recovery_source_id;

    ProcessedSample();
};

typedef std::function<void(const ProcessedSample&)> SampleCallback;

struct ProcessorStats {
    std::uint64_t datagrams;
    std::uint64_t records;
    std::uint64_t orders;
    std::uint64_t executions;
    std::uint64_t heartbeats;
    std::uint64_t known_non_target;
    std::uint64_t duplicates;
    std::uint64_t samples;

    ProcessorStats();
};

// This is deliberately a small SZE-specific bridge around the existing
// decoder and mix153060 runtime. It owns mutable per-instrument runtime state,
// while the model weights and input model are supplied by the caller.
class SzeStreamProcessor {
public:
    // A null model is an explicit factors-only mode; emitted predictions are
    // NaN with prediction_valid=false. A supplied model must already be loaded.
    SzeStreamProcessor(const std::vector<mix153060::StaticInputs>& inputs,
                       const mix153060::Model* model,
                       std::size_t channel_count,
                       const SampleCallback& callback = SampleCallback(),
                       std::uint16_t expected_source_id = 88U);
    ~SzeStreamProcessor();

    SzeStreamProcessor(const SzeStreamProcessor&) = delete;
    SzeStreamProcessor& operator=(const SzeStreamProcessor&) = delete;

    // StreamEvent payload storage is borrowed and is consumed synchronously.
    // Idle events are intentionally ignored: they are transport controls, not
    // exchange events and never cause an implicit model flush.
    void on_event(const deepwin_market_data::StreamEvent& event);

    // Process one selected .szej/SHM canonical record. Recovery is a separate
    // input mode because selected journals intentionally omit wire records.
    void on_recovery(const sze_recovery::CanonicalEvent& event,
                     const void* payload,
                     std::size_t payload_size,
                     const RecoveryTimeContext& time_context);

    // Side-effect-free validation used by journal preflight before callbacks.
    bool validate_recovery_event(const sze_recovery::CanonicalEvent& event,
                                 const void* payload,
                                 std::size_t payload_size,
                                 std::string* error) const;
    bool validate_recovery_time(const sze_recovery::CanonicalEvent& event,
                                const RecoveryTimeContext& time_context,
                                std::string* error) const;
    void set_callback(const SampleCallback& callback);

    bool available() const;
    const std::string& error() const;
    const ProcessorStats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sze_stream

#endif  // T0_PREDICTOR_SZE_STREAM_PROCESSOR_H
