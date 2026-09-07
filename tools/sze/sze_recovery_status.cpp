#include "sze/market_data/SZERecoverable.h"
#include "sze/market_data/SZEHealthState.h"

#include <cstdint>
#include <iostream>

namespace {

template <typename T>
T load_acquire(const T* value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: sze_recovery_status /dev/shm/<ring>" << std::endl;
        return 2;
    }
    sze_recovery::ShmEventRing ring;
    if (!ring.attach(argv[1])) {
        std::cerr << "sze_recovery_status attach_failed=1 path=" << argv[1]
                  << std::endl;
        return 3;
    }
    const sze_recovery::ShmRingHeader* header = ring.header();
    const std::uint64_t published =
        load_acquire(&header->journal_published_offset);
    const std::uint64_t flushed =
        load_acquire(&header->journal_flushed_offset);
    std::cout << "sze_recovery_status"
              << " format_version=" << header->version
              << " trading_day=" << header->trading_day
              << " source_id=" << header->source_id
              << " generation=" << load_acquire(&header->generation)
              << " producer_pid=" << load_acquire(&header->producer_pid)
              << " producer_alive=" << (ring.producer_alive() ? 1 : 0)
              << " continuity=" << load_acquire(&header->continuity_state)
              << " invalid_reason=" << load_acquire(&header->invalid_reason)
              << " invalid_reason_name=" << sze_recovery::invalid_reason_name(
                  static_cast<sze_recovery::InvalidReason>(
                      load_acquire(&header->invalid_reason)))
              << " readiness=" << load_acquire(&header->readiness_state)
              << " journal_degraded="
              << ((load_acquire(&header->flags) &
                   sze_recovery::kRingFlagJournalDegraded) ? 1 : 0)
              << " latest_event_id=" << load_acquire(&header->latest_event_id)
              << " latest_feed_sequence="
              << load_acquire(&header->latest_feed_sequence)
              << " capture_records=" << load_acquire(&header->capture_records)
              << " selected_records=" << load_acquire(&header->selected_records)
              << " duplicate_records=" << load_acquire(&header->duplicate_records)
              << " missing_records=" << load_acquire(&header->missing_records)
              << " malformed_records=" << load_acquire(&header->malformed_records)
              << " journal_events=" << load_acquire(&header->journal_events)
              << " journal_errors=" << load_acquire(&header->journal_errors)
              << " flush_count=" << load_acquire(&header->flush_count)
              << " journal_published_offset=" << published
              << " journal_flushed_offset=" << flushed
              << " flush_lag_bytes=" << (published >= flushed ? published - flushed : 0U)
              << " replay_event_id=" << load_acquire(&header->replay_event_id)
              << " replay_lag=" << load_acquire(&header->replay_lag)
              << " replay_rate_milli=" << load_acquire(&header->replay_rate_milli)
              << " handoff_retries=" << load_acquire(&header->handoff_retries)
              << " ring_overruns=" << load_acquire(&header->ring_overruns)
              << " recovery_elapsed_ms="
              << load_acquire(&header->recovery_elapsed_ms);
    sze_health::HealthReader health;
    if (health.open_capture(sze_health::capture_health_path(argv[1]))) {
        const sze_health::CaptureHealthPage* state = health.capture();
        std::cout << " health_state_present=1"
                  << " feed_health=" << load_acquire(&state->feed_state)
                  << " feed_failure_scope="
                  << load_acquire(&state->feed_failure_scope)
                  << " journal_health=" << load_acquire(&state->journal_state)
                  << " ring_health=" << load_acquire(&state->ring_state)
                  << " health_heartbeat_mono_ns="
                  << load_acquire(&state->heartbeat_mono_ns);
    } else {
        std::cout << " health_state_present=0";
    }
    std::cout << std::endl;
    return 0;
}
