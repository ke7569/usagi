#ifndef SSE_T0_TICK_ORDER_BOOK_H
#define SSE_T0_TICK_ORDER_BOOK_H

#include "sse/market_data/sse_decoded_tick.h"
#include "sse/market_data/sse_primary_decoder.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace sse_tick {

// SSE-specific order state. Do not reuse the SZE order/cancel interpretation:
// SSE merge records carry A/D/T/S and identify both sides of a trade by order
// number. The order book is maintained independently for every security.
enum EventKind { kAdd = 'A', kDelete = 'D', kTrade = 'T', kStatus = 'S' };

struct Order {
    std::uint64_t order_no;
    std::uint32_t price_raw;
    std::uint64_t remaining_qty;
    char side;
    // The exchange wire time is HHMMSScc. Seconds are sufficient for the
    // age features and make the per-order record smaller than a microsecond
    // timestamp.
    std::uint32_t add_time_seconds;
    std::uint32_t age_generation;
};

struct Level {
    std::int64_t price_raw;
    std::uint64_t quantity;
    std::uint64_t order_count;
    std::uint64_t add_time_sum_micros;
    std::uint64_t young_quantity;
};

struct FlowEvent {
    char kind;
    char side;
    std::uint64_t order_no;
    std::uint64_t other_order_no;
    std::uint32_t price_raw;
    std::uint64_t quantity;
};

struct FlowStats {
    std::uint64_t buy_add_qty;
    std::uint64_t sell_add_qty;
    std::uint64_t buy_cancel_qty;
    std::uint64_t sell_cancel_qty;
    std::uint64_t buy_trade_qty;
    std::uint64_t sell_trade_qty;
    std::uint64_t positive_trade_qty;
    std::uint64_t negative_trade_qty;
    std::uint64_t trade_count;
    double trade_turnover;
    // These are accumulated while ticks enter the book. They use the same
    // model-share units as the factor code, so build() does not rescan events.
    double positive_order_flow;
    double negative_order_flow;
    double market_flow;
    std::uint64_t buy_order_qty;
    std::uint64_t sell_order_qty;
    std::uint64_t buy_filled_qty;
    std::uint64_t sell_filled_qty;
    double positive_trade_flow;
    double negative_trade_flow;
    std::vector<FlowEvent> events;
    FlowStats();
    void clear_window();
};

struct ApplyResult {
    bool accepted;
    bool state_event;
    bool book_changed;
    bool sequence_healthy;
    const char* reason;
    ApplyResult();
};

class OrderBook {
public:
    explicit OrderBook(const std::string& security_id);

    ApplyResult apply(const sse_live::TickEvent& event);
    ApplyResult apply(const sse_live::DecodedTick& event);
    // Sets the start-of-window L1 reference used by streamed order-flow
    // factors. The next flow window is accumulated against this reference.
    void set_flow_reference(std::uint32_t bid_price_raw,
                            std::uint32_t ask_price_raw,
                            bool bid_present, bool ask_present);
    bool snapshot(Level* bids, Level* asks, std::size_t depth) const;
    bool full_depth(char side, std::uint64_t now_micros,
                    std::vector<Level>* levels) const;
    // Visit the current ordered levels without materializing a temporary
    // vector. Factor aggregation uses this path on every sample; the vector
    // API above remains for diagnostics and external consumers.
    template <typename Visitor>
    bool for_each_full_depth(char side, std::uint64_t now_micros,
                             Visitor visitor) const;
    // The pointer overload lets a consumer retain and reuse its FlowStats
    // event buffer across windows. The return-by-value API remains available
    // for existing callers.
    void take_flow_window(FlowStats* out);
    FlowStats take_flow_window();
    const std::string& security_id() const { return security_id_; }
    const FlowStats& flow() const { return flow_; }
    std::uint64_t last_tick_index() const { return last_tick_index_; }
    bool has_tick_index() const { return has_tick_index_; }
    std::size_t live_order_count() const { return orders_.size(); }
    std::uint64_t total_trade_qty() const { return total_trade_qty_; }
    double total_trade_turnover() const { return total_trade_turnover_; }
    double last_trade_price() const { return last_trade_price_; }

private:
    struct LevelAggregate {
        std::uint64_t quantity;
        std::uint64_t order_count;
        std::uint64_t add_time_sum_micros;
        mutable std::uint64_t young_quantity;
        LevelAggregate()
            : quantity(0), order_count(0), add_time_sum_micros(0),
              young_quantity(0) {}
    };
    typedef std::map<std::uint32_t, LevelAggregate> LevelMap;

    struct AgeEntry {
        std::uint32_t add_time_seconds;
        std::uint32_t age_generation;
        std::uint64_t order_no;
    };

    bool add_order(std::uint64_t order_no, std::uint32_t price_raw,
                   std::uint64_t quantity, char side, std::uint64_t tick,
                   std::uint64_t time_micros);
    bool delete_order(std::uint64_t order_no, std::uint64_t quantity_hint,
                      std::uint32_t event_price_raw);
    bool trade_order(std::uint64_t order_no, std::uint64_t quantity,
                     std::uint64_t* applied);
    void remove_order_quantity(const Order& order, std::uint64_t quantity,
                               bool remove_order);
    LevelMap& levels(char side);
    const LevelMap& levels(char side) const;
    void refresh_young(std::uint64_t now_micros) const;
    void rebuild_young(std::uint64_t now_micros) const;
    static bool is_young(std::uint32_t now_seconds,
                         std::uint32_t add_time_seconds);
    static bool is_expired(std::uint32_t now_seconds,
                           std::uint32_t add_time_seconds);
    void compact_age_entries() const;

    template <typename Event>
    ApplyResult apply_impl(const Event& event);

    std::string security_id_;
    std::unordered_map<std::uint64_t, Order> orders_;
    LevelMap bid_levels_;
    LevelMap ask_levels_;
    // Entries are appended in exchange-time order. Canceled/filled orders
    // remain as cheap stale references until the expiry cursor passes them;
    // this removes two balanced-tree mutations from every hot update.
    mutable std::vector<AgeEntry> age_entries_;
    mutable std::size_t age_activate_index_;
    mutable std::size_t age_expire_index_;
    std::uint32_t next_age_generation_;
    mutable bool young_cache_initialized_;
    mutable std::uint32_t young_cache_time_seconds_;
    std::uint32_t flow_bid_price_raw_;
    std::uint32_t flow_ask_price_raw_;
    bool flow_bid_present_;
    bool flow_ask_present_;
    FlowStats flow_;
    std::uint64_t last_tick_index_;
    bool has_tick_index_;
    std::uint64_t total_trade_qty_;
    double total_trade_turnover_;
    double last_trade_price_;
};

}  // namespace sse_tick

template <typename Visitor>
bool sse_tick::OrderBook::for_each_full_depth(char side,
                                              std::uint64_t now_micros,
                                              Visitor visitor) const {
    if (side != 'B' && side != 'S') return false;
    refresh_young(now_micros);
    const LevelMap& side_levels = levels(side);
    std::size_t index = 0;
    if (side == 'B') {
        for (LevelMap::const_reverse_iterator it = side_levels.rbegin();
             it != side_levels.rend(); ++it, ++index) {
            visitor(Level{static_cast<std::int64_t>(it->first),
                          it->second.quantity, it->second.order_count,
                          it->second.add_time_sum_micros,
                          it->second.young_quantity}, index);
        }
    } else {
        for (LevelMap::const_iterator it = side_levels.begin();
             it != side_levels.end(); ++it, ++index) {
            visitor(Level{static_cast<std::int64_t>(it->first),
                          it->second.quantity, it->second.order_count,
                          it->second.add_time_sum_micros,
                          it->second.young_quantity}, index);
        }
    }
    return index != 0;
}

#endif  // SSE_T0_TICK_ORDER_BOOK_H
