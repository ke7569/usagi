#ifndef SZ_HP_ORDERBOOK_H
#define SZ_HP_ORDERBOOK_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/contracts/legacy/LFDataStruct.h"
#include "common/market_data/legacy/shsz_full_orderbook_aggregate.h"

namespace sz_hp {

static const uint32_t kPriceScale = 1000U;
static const uint32_t kWindowSec = 30U;
static const size_t kWindowRingSize = kWindowSec + 1U;

uint32_t to_price(double price);
uint32_t parse_event_time_ms(const char* event_time);

enum class OrderType {
    kSelfBest,
    kMarketPrice,
    kLimitPrice,
    kNoop
};

enum class TradeFlag {
    kFill,
    kCancel,
    kNoop
};

struct AdapterDiagnostic {
    enum Code {
        kNone,
        kNullInput,
        kInvalidInstrument,
        kInvalidQuantity,
        kInvalidSequence,
        kInvalidTime,
        kBookFailure
    };

    std::array<char, 31> instrument;
    uint64_t event_index;
    Code code;
    int64_t sequence;
    int64_t bid_id;
    int64_t ask_id;
    std::string reason;

    AdapterDiagnostic();
    void clear();
};

struct OrderEvent {
    std::array<char, 31> instrument;
    uint64_t sequence;
    uint64_t business_index;
    uint64_t order_id;
    uint32_t event_time_ms;
    uint32_t price;
    double raw_price;
    int64_t quantity;
    bool is_buy;
    OrderType type;
    char raw_order_type;

    OrderEvent();
};

struct TradeEvent {
    std::array<char, 31> instrument;
    uint64_t sequence;
    uint64_t business_index;
    uint64_t bid_id;
    uint64_t ask_id;
    uint32_t event_time_ms;
    uint32_t price;
    double raw_price;
    int64_t quantity;
    TradeFlag flag;
    char raw_trade_flag;

    TradeEvent();
};

struct MarketObservation {
    std::array<char, 31> instrument;
    uint32_t event_time_ms;
    double last_price;
    double total_volume;
    double turnover;
    double upper_limit_price;
    double lower_limit_price;
    std::array<double, 10> bid_price;
    std::array<double, 10> ask_price;
    std::array<double, 10> bid_volume;
    std::array<double, 10> ask_volume;
    bool valid;

    MarketObservation();
    double fast_mid_price() const;
    bool has_level_one() const;
};

class EventAdapter {
public:
    static bool normalize_order(const LFL2OrderField& source,
                                OrderEvent* destination,
                                AdapterDiagnostic* diagnostic = 0,
                                uint64_t event_index = 0);
    static bool normalize_trade(const LFL2TradeField& source,
                                TradeEvent* destination,
                                AdapterDiagnostic* diagnostic = 0,
                                uint64_t event_index = 0);
    static bool normalize_observation(const LFL2MarketDataField& source,
                                      MarketObservation* destination,
                                      AdapterDiagnostic* diagnostic = 0,
                                      uint64_t event_index = 0);
};

class SlidingWindow {
public:
    typedef int64_t Volume;

    SlidingWindow();

    bool add(uint32_t event_time_ms, Volume volume);
    bool erase(uint32_t event_time_ms, Volume volume);
    Volume total(uint32_t now_time_ms) const;
    void clear();
    uint32_t now_sec() const;

private:
    struct Bucket {
        uint32_t tick;
        Volume sum;
    };

    Bucket& bucket(uint32_t tick) const;
    bool in_window(uint32_t tick) const;
    void advance_to(uint32_t tick) const;

    mutable std::array<Bucket, kWindowRingSize> ring_;
    mutable Volume total_;
    mutable uint32_t now_sec_;
};

struct QuoteOrder {
    int64_t volume;
    int64_t trade_quantity;
    uint64_t timestamp;
    bool is_buy;

    QuoteOrder();
    QuoteOrder(int64_t quantity, uint64_t timestamp_ms, bool buy);
    int64_t left() const;
    bool on_trade(int64_t quantity);
};

class Level {
public:
    explicit Level(uint32_t price = 0);

    bool add_order(uint64_t order_id,
                   int64_t quantity,
                   uint64_t timestamp_ms,
                   bool is_buy);
    bool on_trade(uint64_t order_id, int64_t quantity);
    bool cancel_order(uint64_t order_id);

    uint32_t price() const;
    int64_t volume() const;
    uint64_t total_timestamp() const;
    size_t order_count() const;
    int64_t window_volume_now() const;
    bool empty() const;
    int64_t window_volume(uint32_t now_time_ms) const;
    const std::unordered_map<uint64_t, QuoteOrder>& orders() const;

private:
    uint32_t price_;
    int64_t volume_;
    uint64_t total_timestamp_;
    std::unordered_map<uint64_t, QuoteOrder> orders_;
    mutable SlidingWindow window_;
};

class Depth {
public:
    explicit Depth(bool buy_side = false);

    Level* find(uint32_t price);
    const Level* find(uint32_t price) const;
    Level& find_or_create(uint32_t price);
    bool erase(uint32_t price);
    size_t count() const;
    Level* first_level();
    const Level* first_level() const;
    Level* last_level();
    const Level* last_level() const;
    const std::vector<Level>& levels() const;
    void clear();

private:
    size_t lower_bound_index(uint32_t price) const;

    bool buy_side_;
    std::vector<Level> levels_;
};

class OrderBook {
public:
    explicit OrderBook(const std::string& instrument = std::string());

    bool update_order(const OrderEvent& event);
    bool update_trade(const TradeEvent& event);
    // Reject an event that cannot be normalized into the HP contract. This is
    // intentionally a failure-only path; successful event mutation never calls it.
    void reject(uint64_t sequence, const char* reason);
    bool add_order(uint64_t order_id,
                   bool is_buy,
                   uint32_t price,
                   int64_t quantity,
                   uint64_t timestamp_ms,
                   OrderType type = OrderType::kLimitPrice);
    bool cancel_order(uint64_t order_id,
                     uint32_t price,
                     bool is_buy,
                     uint64_t sequence,
                     uint64_t timestamp_ms);

    const std::string& instrument() const;
    const Depth& bids() const;
    const Depth& asks() const;
    bool available() const;
    uint64_t failure_sequence() const;
    const std::string& failure_reason() const;
    size_t order_count() const;
    int64_t best_bid_volume() const;
    int64_t best_ask_volume() const;
    uint32_t best_bid_price() const;
    uint32_t best_ask_price() const;
    uint32_t mid_price() const;

    ShSzFullOb snapshot_full_orderbook_aggregate(uint32_t now_time_ms) const;
    double young_orderbook_imbalance(int max_basis_points, uint32_t now_time_ms) const;
    double fix_dist_hermes(int max_basis_points, uint32_t now_time_ms) const;
    std::string digest() const;

    void clear();

private:
    bool fill_side(Depth* depth,
                   uint64_t order_id,
                   int64_t quantity,
                   uint64_t sequence,
                   bool buy_side);
    bool fail(uint64_t sequence, const char* reason);

    std::string instrument_;
    Depth bids_;
    Depth asks_;
    bool available_;
    uint64_t failure_sequence_;
    std::string failure_reason_;
    uint32_t last_event_time_ms_;
};

}  // namespace sz_hp

#endif  // SZ_HP_ORDERBOOK_H
