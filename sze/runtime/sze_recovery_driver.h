#ifndef T0_PREDICTOR_SZE_RECOVERY_DRIVER_H
#define T0_PREDICTOR_SZE_RECOVERY_DRIVER_H

#include <cstddef>
#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sze/runtime/sze_stream_processor.h"

namespace sze_recovery {
class ReplayHandoffConsumer;
}

namespace sze_stream {

struct SzeRecoveryJournalConfig {
    std::string directory;
    std::string prefix;
    std::uint32_t trading_day;
    std::uint16_t source_id;
    std::uint64_t segment_bytes;
    std::uint32_t max_payload_bytes;
    std::uint64_t expected_generation;
    std::string shm_path;
    std::uint64_t handoff_timeout_ms;

    SzeRecoveryJournalConfig();
};

struct SzeRecoveryDriverStats {
    std::uint64_t records;
    std::uint64_t payload_bytes;
    std::uint64_t handoff_events;

    SzeRecoveryDriverStats();
};

// Strict journal reader plus an explicit, read-only ReplayHandoffConsumer
// bridge. Handoff readiness is only reported after a validated ring event.
class SzeRecoveryDriver {
public:
    explicit SzeRecoveryDriver(SzeStreamProcessor* processor);
    ~SzeRecoveryDriver();

    SzeRecoveryDriver(const SzeRecoveryDriver&) = delete;
    SzeRecoveryDriver& operator=(const SzeRecoveryDriver&) = delete;

    // The journal is fully preflighted before the processor receives a record.
    // No EOF flush or synthetic idle event is generated.
    bool replay(const SzeRecoveryJournalConfig& config,
                const RecoveryTimeContext& time_context);

    // Consume journal then wait for the existing producer's ring cutover. The
    // ring is read-only from this driver; successful return means one valid
    // ring event was processed while producer/epoch/continuity checks held.
    bool replay_handoff(const SzeRecoveryJournalConfig& config,
                        const RecoveryTimeContext& time_context);
    // Continue a successful handoff without changing readiness semantics.
    bool poll_handoff();
    // Observer runs on the processing thread after preflight, before mutation.
    void set_event_observer(const std::function<void(const sze_recovery::CanonicalEvent&)>& observer);
    void request_stop();

    bool available() const;
    // Journal-only analysis never grants a live handoff/readiness state.
    bool live_ready() const;
    const std::string& error() const;
    const SzeRecoveryDriverStats& stats() const;

private:
    SzeStreamProcessor* processor_;
    SzeRecoveryDriverStats stats_;
    bool available_;
    std::string error_;
    std::unique_ptr<sze_recovery::ReplayHandoffConsumer> handoff_consumer_;
    std::vector<unsigned char> handoff_payload_;
    RecoveryTimeContext handoff_time_context_;
    std::uint64_t handoff_generation_;
    std::uint64_t handoff_first_event_id_;
    std::uint64_t handoff_previous_event_id_;
    std::uint32_t handoff_trading_day_;
    std::uint16_t handoff_source_id_;
    bool handoff_active_;
    bool live_ready_;
    std::atomic<bool> stop_requested_;
    std::function<void(const sze_recovery::CanonicalEvent&)> event_observer_;
};

}  // namespace sze_stream

#endif  // T0_PREDICTOR_SZE_RECOVERY_DRIVER_H
