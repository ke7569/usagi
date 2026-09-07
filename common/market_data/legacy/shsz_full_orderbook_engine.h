#ifndef SHSZ_FULL_ORDERBOOK_ENGINE_H
#define SHSZ_FULL_ORDERBOOK_ENGINE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>

#include "common/contracts/legacy/RawDataStruct.h"
#include "common/market_data/legacy/shsz_full_orderbook_aggregate.h"

class ShSzSlidingWindowVolume {
public:
    static const uint32_t kWindowSec = 30;
    static const uint32_t kRingSize = kWindowSec + 1;

    ShSzSlidingWindowVolume();

    bool add(uint32_t event_time_ms, int volume);
    bool erase(uint32_t event_time_ms, int volume);
    int total(uint32_t now_time_ms) const;
    void clear();

private:
    struct Bucket {
        uint32_t tick_sec;
        int volume;
    };

    void advance_to(uint32_t now_sec) const;
    bool in_window(uint32_t event_sec) const;
    Bucket& bucket(uint32_t tick_sec) const;

    mutable std::array<Bucket, kRingSize> mRing;
    mutable int mTotalVolume;
    mutable uint32_t mNowSec;
};

struct ShSzFullOrderRecord {
    long quote_tag;
    int price;
    int total_volume;
    int remaining_volume;
    bool is_sell;
    uint32_t create_time_ms;

    ShSzFullOrderRecord();
};

struct ShSzVisibleBookLevel {
    int price;
    int total_volume;
    int order_count;
    uint64_t total_create_time_ms;
    int window_volume;
    bool valid;

    ShSzVisibleBookLevel();
};

struct ShSzVisibleBook {
    std::array<ShSzVisibleBookLevel, PRICE_LEVEL> bids;
    std::array<ShSzVisibleBookLevel, PRICE_LEVEL> asks;
};

struct ShSzBookAggregateSummary {
    int best_price;
    int best_volume;
    int level_count;
    int total_volume;
    int total_order_count;
    int total_window_volume;
    int max_level_volume;
    int max_level_price;
    uint64_t total_create_time_ms;
    double total_amount;
    bool valid;

    ShSzBookAggregateSummary();
};

struct ShSzFullOrderBookSummary {
    double instrument_id_value;
    int mid_price;
    int total_order_count;
    ShSzBookAggregateSummary bid;
    ShSzBookAggregateSummary ask;

    ShSzFullOrderBookSummary();
};

struct ShSzFullOrderBookStateSnapshot {
    ShSzVisibleBook visible_book;
    ShSzFullOrderBookSummary summary;
};

class ShSzFullOrderBookLevel {
public:
    explicit ShSzFullOrderBookLevel(int price = 0);

    bool add_order(const ShSzFullOrderRecord& order);
    bool reduce_order(long quote_tag, int volume, bool erase_when_flat, bool* removed_order = 0);
    bool remove_order(long quote_tag);
    bool get_order(long quote_tag, ShSzFullOrderRecord* out) const;

    int price() const;
    int total_volume() const;
    int order_count() const;
    uint64_t total_create_time_ms() const;
    double total_amount() const;
    int window_volume(uint32_t now_time_ms) const;
    bool empty() const;
    void clear();

private:
    int mPrice;
    int mTotalVolume;
    uint64_t mTotalCreateTimeMs;
    std::unordered_map<long, ShSzFullOrderRecord> mOrders;
    ShSzSlidingWindowVolume mWindowVolume;
};

class ShSzFullOrderBookEngine {
public:
    ShSzFullOrderBookEngine();

    void set_instrument_id_value(double instrument_id_value);
    double instrument_id_value() const;

    bool add_order(long quote_tag, bool is_sell, int price, int volume, uint32_t event_time_ms);
    bool execute_order(long quote_tag, int volume);
    bool cancel_order(long quote_tag, int volume);
    bool remove_order(long quote_tag);
    bool get_order(long quote_tag, ShSzFullOrderRecord* out) const;

    bool has_order(long quote_tag) const;
    size_t bid_level_count() const;
    size_t ask_level_count() const;
    size_t order_count() const;
    int best_bid_price() const;
    int best_ask_price() const;
    int mid_price() const;
    int best_bid_volume() const;
    int best_ask_volume() const;
    ShSzVisibleBook snapshot_visible_book(uint32_t now_time_ms) const;
    ShSzBookAggregateSummary snapshot_bid_summary(uint32_t now_time_ms) const;
    ShSzBookAggregateSummary snapshot_ask_summary(uint32_t now_time_ms) const;
    ShSzFullOrderBookSummary snapshot_summary(uint32_t now_time_ms) const;
    ShSzFullOrderBookStateSnapshot snapshot_state(uint32_t now_time_ms) const;
    ShSzFullOb snapshot_full_orderbook_aggregate(uint32_t now_time_ms) const;
    double young_orderbook_imbalance(int max_basis_points, uint32_t now_time_ms) const;
    double fix_dist_hermes(int max_basis_points, uint32_t now_time_ms) const;
    ShSzBookAggregateSummary summarize_best_n_levels(bool is_sell,
                                                     size_t max_levels,
                                                     uint32_t now_time_ms) const;
    ShSzBookAggregateSummary summarize_levels_by_mid_bp(bool is_sell,
                                                        int mid_price,
                                                        int max_basis_points,
                                                        uint32_t now_time_ms) const;

    void clear();

    static uint32_t parse_event_time_ms(const char* event_time);

private:
    struct OrderLocator {
        bool is_sell;
        int price;
    };

    typedef std::map<int, ShSzFullOrderBookLevel> AskBook;
    typedef std::map<int, ShSzFullOrderBookLevel> BidBook;

    bool reduce_order(long quote_tag, int volume, bool erase_when_flat);
    void rescan_bid_max_level();
    void rescan_ask_max_level();
    void update_bid_max_level_on_add(int price, int level_volume);
    void update_ask_max_level_on_add(int price, int level_volume);
    void refresh_bid_side_cache(uint32_t now_time_ms);
    void refresh_ask_side_cache(uint32_t now_time_ms);
    void refresh_side_cache(bool is_sell, uint32_t now_time_ms);
    ShSzFullOrderBookSummary snapshot_lightweight_summary() const;
    bool fill_visible_side(const std::map<int, ShSzFullOrderBookLevel>& book,
                           bool is_bid,
                           std::array<ShSzVisibleBookLevel, PRICE_LEVEL>* out,
                           uint32_t now_time_ms) const;
    bool refresh_visible_window_volume(const std::map<int, ShSzFullOrderBookLevel>& book,
                                       std::array<ShSzVisibleBookLevel, PRICE_LEVEL>* out,
                                       uint32_t now_time_ms) const;

    double mInstrumentIdValue;
    AskBook mAskBook;
    BidBook mBidBook;
    std::unordered_map<long, OrderLocator> mOrderLocators;
    int mBidOrderCount;
    int mAskOrderCount;
    int mBidMaxLevelVolume;
    int mAskMaxLevelVolume;
    double mBidMaxLevelPrice;
    double mAskMaxLevelPrice;
    ShSzVisibleBook mVisibleBookCache;
    ShSzBookAggregateSummary mBidLightSummary;
    ShSzBookAggregateSummary mAskLightSummary;
};

#endif // SHSZ_FULL_ORDERBOOK_ENGINE_H
