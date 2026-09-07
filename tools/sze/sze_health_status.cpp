#include "sze/market_data/SZEHealthState.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: sze_health_status RING_PATH SHARD_COUNT [TD_PATH] [SYMBOL]"
                  << std::endl;
        return 2;
    }
    const std::string ring_path = argv[1];
    const std::uint32_t shard_count = static_cast<std::uint32_t>(
        std::strtoul(argv[2], 0, 10));
    const std::string td_path = argc >= 4 ? argv[3] :
        sze_health::td_health_path(ring_path);
    const std::uint32_t symbol_id = argc == 5 ?
        sze_health::parse_symbol_id(argv[4]) : 1000000U;

    sze_health::HealthReader capture_reader;
    if (!capture_reader.open_capture(sze_health::capture_health_path(ring_path))) {
        std::cerr << "sze_health_status capture_health_missing=1" << std::endl;
        return 3;
    }
    const sze_health::CaptureHealthPage* capture = capture_reader.capture();
    const bool capture_alive = sze_health::health_writer_alive(
        capture->writer_pid) && sze_health::health_heartbeat_fresh(
        capture->heartbeat_mono_ns);
    std::uint32_t live_shards = 0U;
    std::uint32_t failed_shards = 0U;
    std::uint32_t symbols = 0U;
    std::uint32_t invalid_symbols = 0U;
    const sze_health::RecoveryShardHealthPage* owner = 0;
    const sze_health::SymbolHealthRecord* target = 0;
    sze_health::HealthReader owner_reader;
    for (std::uint32_t shard = 0; shard < shard_count; ++shard) {
        sze_health::HealthReader reader;
        if (!reader.open_shard(sze_health::shard_health_path(ring_path, shard))) {
            ++failed_shards;
            continue;
        }
        const sze_health::RecoveryShardHealthPage* page = reader.shard();
        symbols += page->symbol_count;
        invalid_symbols += page->invalid_symbol_count;
        if (sze_health::health_writer_alive(page->writer_pid) &&
            sze_health::health_heartbeat_fresh(page->heartbeat_mono_ns) &&
            page->health_state == sze_health::kHealthHealthy &&
            page->readiness == sze_recovery::kReadinessLiveReady) ++live_shards;
        else ++failed_shards;
        if (symbol_id < 1000000U && reader.symbol(symbol_id)) {
            owner_reader.close();
            if (owner_reader.open_shard(
                    sze_health::shard_health_path(ring_path, shard))) {
                owner = owner_reader.shard();
                target = owner_reader.symbol(symbol_id);
            }
        }
    }
    sze_health::HealthReader td_reader;
    const bool td_present = td_reader.open_td(td_path);
    const bool td_alive = td_present && sze_health::health_writer_alive(
        td_reader.td()->writer_pid) && sze_health::health_heartbeat_fresh(
        td_reader.td()->heartbeat_mono_ns);
    sze_health::HealthState aggregate = sze_health::kHealthHealthy;
    if (!capture_alive || capture->feed_state != sze_health::kHealthHealthy ||
        capture->ring_state != sze_health::kHealthHealthy ||
        failed_shards != 0U) {
        aggregate = sze_health::kHealthFailed;
    } else if (capture->journal_state != sze_health::kHealthHealthy ||
               invalid_symbols != 0U || !td_alive ||
               td_reader.td()->td_state != sze_health::kTdReady) {
        aggregate = sze_health::kHealthDegraded;
    }
    std::cout << "sze_health_status"
              << " global_health=" << aggregate
              << " global_health_name="
              << sze_health::health_state_name(aggregate)
              << " feed_health=" << capture->feed_state
              << " capture_alive=" << (capture_alive ? 1 : 0)
              << " feed_scope=" << capture->feed_failure_scope
              << " feed_reason=" << capture->feed_invalid_reason
              << " journal_health=" << capture->journal_state
              << " journal_errors=" << capture->journal_errors
              << " ring_health=" << capture->ring_state
              << " ring_overruns=" << capture->ring_overruns
              << " live_shards=" << live_shards
              << " shard_count=" << shard_count
              << " failed_shards=" << failed_shards
              << " symbols=" << symbols
              << " invalid_symbols=" << invalid_symbols;

    if (td_present) {
        std::cout << " td_health=" << td_reader.td()->td_state
                  << " td_alive=" << (td_alive ? 1 : 0)
                  << " td_logged_in=" << td_reader.td()->logged_in
                  << " td_account_ready=" << td_reader.td()->account_ready
                  << " td_positions_ready=" << td_reader.td()->positions_ready;
        if (owner && target) {
            sze_health::TradePolicy policy;
            const sze_health::TradeDecision decision = sze_health::derive_can_trade(
                *capture, *owner, target, *td_reader.td(), policy);
            std::cout << " symbol=" << symbol_id
                      << " owner_shard=" << owner->shard_id
                      << " book_validity=" << target->book_validity
                      << " prediction_health=" << target->prediction_state
                      << " can_new_risk=" << (decision.allow_new_risk ? 1 : 0)
                      << " can_reduce_risk="
                      << (decision.allow_risk_reduction ? 1 : 0)
                      << " trade_reason="
                      << sze_health::trade_block_reason_name(decision.reason);
        }
    } else {
        std::cout << " td_health=0";
    }
    std::cout << std::endl;
    return failed_shards == 0U ? 0 : 4;
}
