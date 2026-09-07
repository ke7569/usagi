#include "sze/market_data/SZEHealthState.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

int main()
{
    char root[128];
    std::snprintf(root, sizeof(root), "/tmp/sze_health_%ld",
                  static_cast<long>(::getpid()));
    const std::string capture_path = std::string(root) + ".capture";
    const std::string shard_path = std::string(root) + ".shard";
    const std::string td_path = std::string(root) + ".td";

    sze_health::CaptureHealthWriter capture_writer;
    assert(capture_writer.create(capture_path, 20260814U, 88U, 77U, true));
    capture_writer.publish_feed(sze_health::kHealthHealthy,
        sze_recovery::kInvalidNone, sze_health::kScopeNone, 0U, 100U, 900U);
    capture_writer.publish_journal(sze_health::kHealthHealthy, 0U, 0U);
    capture_writer.publish_ring(sze_health::kHealthHealthy, 0U, 0U);

    std::vector<std::uint32_t> symbols;
    symbols.push_back(1U);
    symbols.push_back(807U);
    sze_health::RecoveryShardHealthWriter shard_writer;
    assert(shard_writer.create(shard_path, 20260814U, 88U, 77U,
                               2U, 8U, symbols, true));
    shard_writer.publish_shard(sze_recovery::kReadinessLiveReady,
        sze_health::kHealthHealthy, 0U, 100U, 50U, 900U,
        0U, 1000000U, 0U, 50U);
    assert(shard_writer.publish_symbol(1U, sze_health::kBookValid,
        sze_health::kPredictionHealthy, 0U, 0U, 100U, 10U, 11U, 123.0));

    sze_health::TdHealthWriter td_writer;
    assert(td_writer.create(td_path, 20260814U, 180U, 77U, true));
    td_writer.publish(sze_health::kTdReady, true, true, true, 0U,
                      1U, 2U, 3U, 0U);

    sze_health::HealthReader capture_reader;
    sze_health::HealthReader shard_reader;
    sze_health::HealthReader td_reader;
    assert(capture_reader.open_capture(capture_path));
    assert(shard_reader.open_shard(shard_path));
    assert(td_reader.open_td(td_path));
    const sze_health::SymbolHealthRecord* symbol = shard_reader.symbol(1U);
    assert(symbol != 0);
    sze_health::TradePolicy policy;
    sze_health::TradeDecision decision = sze_health::derive_can_trade(
        *capture_reader.capture(), *shard_reader.shard(), symbol,
        *td_reader.td(), policy);
    assert(decision.allow_new_risk);
    assert(decision.allow_risk_reduction);
    assert(decision.reason == sze_health::kTradeAllowed);

    capture_writer.publish_journal(sze_health::kHealthDegraded, 5U, 1U);
    decision = sze_health::derive_can_trade(
        *capture_reader.capture(), *shard_reader.shard(), symbol,
        *td_reader.td(), policy);
    assert(!decision.allow_new_risk);
    assert(decision.allow_risk_reduction);
    assert(decision.reason == sze_health::kTradeJournalDegraded);

    capture_writer.publish_feed(sze_health::kHealthFailed,
        sze_recovery::kInvalidForwardGap, sze_health::kScopeGlobalFeed,
        0U, 101U, 902U);
    decision = sze_health::derive_can_trade(
        *capture_reader.capture(), *shard_reader.shard(), symbol,
        *td_reader.td(), policy);
    assert(!decision.allow_new_risk);
    assert(!decision.allow_risk_reduction);
    assert(decision.reason == sze_health::kTradeFeedNotHealthy);

    td_reader.close();
    shard_reader.close();
    capture_reader.close();
    td_writer.close();
    shard_writer.close();
    capture_writer.close();
    ::unlink(capture_path.c_str());
    ::unlink(shard_path.c_str());
    ::unlink(td_path.c_str());
    return 0;
}
