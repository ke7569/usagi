#ifndef DEEPWIN_SZE_HEALTH_STATE_H
#define DEEPWIN_SZE_HEALTH_STATE_H

#include "sze/market_data/SZERecoverable.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sze_health {

static const std::uint32_t kHealthFormatVersion = 1U;
static const std::size_t kHealthPageBytes = 4096U;

enum HealthState {
    kHealthUnknown = 0,
    kHealthHealthy = 1,
    kHealthDegraded = 2,
    kHealthFailed = 3,
};

enum FailureScope {
    kScopeNone = 0,
    kScopeGlobalFeed = 1,
    kScopeChannel = 2,
    kScopeShard = 3,
    kScopeSymbol = 4,
};

enum BookValidity {
    kBookUnknown = 0,
    kBookValid = 1,
    kBookInvalid = 2,
};

enum PredictionState {
    kPredictionUnknown = 0,
    kPredictionHealthy = 1,
    kPredictionStale = 2,
    kPredictionInvalid = 3,
};

enum TdState {
    kTdUnknown = 0,
    kTdConnecting = 1,
    kTdReady = 2,
    kTdDegraded = 3,
    kTdFailed = 4,
};

enum TradeBlockReason {
    kTradeAllowed = 0,
    kTradeFeedNotHealthy = 1,
    kTradeRingNotHealthy = 2,
    kTradeShardNotLive = 3,
    kTradeBookInvalid = 4,
    kTradePredictionInvalid = 5,
    kTradeTdNotReady = 6,
    kTradeJournalDegraded = 7,
    kTradeSymbolNotOwned = 8,
};

struct alignas(64) CaptureHealthPage {
    char magic[8];
    std::uint64_t generation;
    std::uint64_t heartbeat_mono_ns;
    std::uint64_t latest_global_event_id;
    std::uint64_t latest_feed_sequence;
    std::uint64_t journal_errors;
    std::uint64_t ring_overruns;
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint32_t writer_pid;
    std::uint32_t feed_state;
    std::uint32_t feed_invalid_reason;
    std::uint32_t feed_failure_scope;
    std::uint32_t feed_channel;
    std::uint32_t journal_state;
    std::uint32_t journal_error;
    std::uint32_t ring_state;
    std::uint32_t ring_reason;
    std::uint32_t flags;
    std::uint8_t reserved[3984];
};

struct alignas(64) RecoveryShardHealthPage {
    char magic[8];
    std::uint64_t generation;
    std::uint64_t heartbeat_mono_ns;
    std::uint64_t last_global_event_id;
    std::uint64_t last_shard_event_id;
    std::uint64_t latest_feed_sequence;
    std::uint64_t replay_lag;
    std::uint64_t replay_rate_milli;
    std::uint64_t ring_overruns;
    std::uint64_t events_processed;
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint32_t writer_pid;
    std::uint32_t shard_id;
    std::uint32_t shard_count;
    std::uint32_t readiness;
    std::uint32_t health_state;
    std::uint32_t invalid_reason;
    std::uint32_t symbol_count;
    std::uint32_t invalid_symbol_count;
    std::uint32_t symbol_record_bytes;
    std::uint32_t flags;
    std::uint8_t reserved[3960];
};

struct alignas(64) SymbolHealthRecord {
    std::uint64_t last_global_event_id;
    std::uint64_t last_book_update_mono_ns;
    std::uint64_t last_prediction_mono_ns;
    std::uint64_t last_turnover_bits;
    std::uint32_t symbol_id;
    std::uint32_t book_validity;
    std::uint32_t prediction_state;
    std::uint32_t book_reason;
    std::uint32_t prediction_reason;
    std::uint32_t flags;
    std::uint8_t reserved[8];
};

struct alignas(64) TdHealthPage {
    char magic[8];
    std::uint64_t generation;
    std::uint64_t heartbeat_mono_ns;
    std::uint64_t account_query_mono_ns;
    std::uint64_t position_query_mono_ns;
    std::uint64_t last_order_callback_mono_ns;
    std::uint64_t rejected_orders;
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t trading_day;
    std::uint32_t source_id;
    std::uint32_t writer_pid;
    std::uint32_t td_state;
    std::uint32_t logged_in;
    std::uint32_t account_ready;
    std::uint32_t positions_ready;
    std::uint32_t last_error;
    std::uint32_t flags;
    std::uint8_t reserved[3996];
};

static_assert(sizeof(CaptureHealthPage) == kHealthPageBytes,
              "unexpected capture health ABI");
static_assert(sizeof(RecoveryShardHealthPage) == kHealthPageBytes,
              "unexpected shard health ABI");
static_assert(sizeof(SymbolHealthRecord) == 64U,
              "unexpected symbol health ABI");
static_assert(sizeof(TdHealthPage) == kHealthPageBytes,
              "unexpected td health ABI");

class CaptureHealthWriter {
public:
    CaptureHealthWriter();
    ~CaptureHealthWriter();
    bool create(const std::string& path, std::uint32_t trading_day,
                std::uint32_t source_id, std::uint64_t generation,
                bool replace_stale);
    void close();
    void publish_feed(HealthState state, sze_recovery::InvalidReason reason,
                      FailureScope scope, std::uint32_t channel,
                      std::uint64_t event_id, std::uint64_t feed_sequence);
    void publish_journal(HealthState state, std::uint32_t error,
                         std::uint64_t journal_errors);
    void publish_ring(HealthState state, std::uint32_t reason,
                      std::uint64_t overruns);
    void heartbeat();
    const CaptureHealthPage* page() const { return page_; }
private:
    int fd_;
    CaptureHealthPage* page_;
};

class RecoveryShardHealthWriter {
public:
    RecoveryShardHealthWriter();
    ~RecoveryShardHealthWriter();
    bool create(const std::string& path, std::uint32_t trading_day,
                std::uint32_t source_id, std::uint64_t generation,
                std::uint32_t shard_id, std::uint32_t shard_count,
                const std::vector<std::uint32_t>& symbols,
                bool replace_stale);
    void close();
    void publish_shard(sze_recovery::ReadinessState readiness,
                       HealthState health, std::uint32_t invalid_reason,
                       std::uint64_t global_event_id,
                       std::uint64_t shard_event_id,
                       std::uint64_t feed_sequence,
                       std::uint64_t replay_lag,
                       std::uint64_t replay_rate_milli,
                       std::uint64_t ring_overruns,
                       std::uint64_t events_processed);
    bool publish_symbol(std::uint32_t symbol_id, BookValidity book,
                        PredictionState prediction, std::uint32_t book_reason,
                        std::uint32_t prediction_reason,
                        std::uint64_t global_event_id,
                        std::uint64_t book_update_mono_ns,
                        std::uint64_t prediction_mono_ns,
                        double turnover);
    bool publish_book(std::uint32_t symbol_id, BookValidity book,
                      std::uint32_t reason, std::uint64_t global_event_id,
                      std::uint64_t update_mono_ns);
    bool publish_book_valid_once(std::uint32_t symbol_id,
                                 std::uint64_t global_event_id,
                                 std::uint64_t update_mono_ns);
    bool publish_prediction(std::uint32_t symbol_id, PredictionState state,
                            std::uint32_t reason,
                            std::uint64_t prediction_mono_ns,
                            double turnover);
    const RecoveryShardHealthPage* page() const { return page_; }
    const SymbolHealthRecord* symbols() const { return symbols_; }
private:
    int find_symbol(std::uint32_t symbol_id) const;
    int fd_;
    unsigned char* mapping_;
    std::size_t mapping_bytes_;
    RecoveryShardHealthPage* page_;
    SymbolHealthRecord* symbols_;
    std::vector<std::int32_t> symbol_index_;
};

class TdHealthWriter {
public:
    TdHealthWriter();
    ~TdHealthWriter();
    bool create(const std::string& path, std::uint32_t trading_day,
                std::uint32_t source_id, std::uint64_t generation,
                bool replace_stale);
    void close();
    void publish(TdState state, bool logged_in, bool account_ready,
                 bool positions_ready, std::uint32_t last_error,
                 std::uint64_t account_query_ns,
                 std::uint64_t position_query_ns,
                 std::uint64_t order_callback_ns,
                 std::uint64_t rejected_orders);
private:
    int fd_;
    TdHealthPage* page_;
};

class HealthReader {
public:
    HealthReader();
    ~HealthReader();
    bool open_capture(const std::string& path);
    bool open_shard(const std::string& path);
    bool open_td(const std::string& path);
    void close();
    const CaptureHealthPage* capture() const;
    const RecoveryShardHealthPage* shard() const;
    const SymbolHealthRecord* symbol(std::uint32_t symbol_id) const;
    const TdHealthPage* td() const;
private:
    enum Kind { kNone, kCapture, kShard, kTd };
    bool map(const std::string& path, Kind kind);
    int fd_;
    const unsigned char* mapping_;
    std::size_t mapping_bytes_;
    Kind kind_;
};

struct TradePolicy {
    TradePolicy() : require_durable_journal_for_new_risk(true) {}
    bool require_durable_journal_for_new_risk;
};

struct TradeDecision {
    TradeDecision() : allow_new_risk(false), allow_risk_reduction(false),
                      reason(kTradeFeedNotHealthy) {}
    bool allow_new_risk;
    bool allow_risk_reduction;
    TradeBlockReason reason;
};

TradeDecision derive_can_trade(const CaptureHealthPage& capture,
                               const RecoveryShardHealthPage& shard,
                               const SymbolHealthRecord* symbol,
                               const TdHealthPage& td,
                               const TradePolicy& policy);

const char* health_state_name(HealthState state);
const char* trade_block_reason_name(TradeBlockReason reason);
bool health_writer_alive(std::uint32_t pid);
bool health_heartbeat_fresh(std::uint64_t heartbeat_mono_ns,
                            std::uint64_t max_age_ns = 5000000000ULL);
std::uint32_t parse_symbol_id(const char* symbol);
std::string capture_health_path(const std::string& ring_path);
std::string shard_health_path(const std::string& ring_path,
                              std::uint32_t shard_id);
std::string td_health_path(const std::string& ring_path);

}  // namespace sze_health

#endif
