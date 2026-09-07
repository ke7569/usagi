#ifndef SSE_T0_TICK_ORDER_BOOK_H
#define SSE_T0_TICK_ORDER_BOOK_H

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
    std::uint64_t add_tick_index;
    std::uint64_t add_time_micros;
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
    bool snapshot(Level* bids, Level* asks, std::size_t depth) const;
    bool full_depth(char side, std::uint64_t now_micros,
                    std::vector<Level>* levels) const;
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
    typedef std::map<std::uint32_t, std::uint64_t> QuantityMap;
    typedef std::map<std::uint32_t, std::uint64_t> CountMap;

    bool add_order(std::uint64_t order_no, std::uint32_t price_raw,
                   std::uint64_t quantity, char side, std::uint64_t tick,
                   std::uint64_t time_micros);
    bool delete_order(std::uint64_t order_no, std::uint64_t quantity_hint,
                      std::uint32_t event_price_raw);
    bool trade_order(std::uint64_t order_no, std::uint64_t quantity,
                     std::uint64_t* applied);
    void add_level(std::uint32_t price, std::uint64_t quantity, char side);
    void remove_level(std::uint32_t price, std::uint64_t quantity, char side);
    QuantityMap& quantities(char side);
    const QuantityMap& quantities(char side) const;
    CountMap& counts(char side);
    const CountMap& counts(char side) const;

    std::string security_id_;
    std::unordered_map<std::uint64_t, Order> orders_;
    QuantityMap bid_qty_;
    QuantityMap ask_qty_;
    CountMap bid_count_;
    CountMap ask_count_;
    FlowStats flow_;
    std::uint64_t last_tick_index_;
    bool has_tick_index_;
    std::uint64_t total_trade_qty_;
    double total_trade_turnover_;
    double last_trade_price_;
};

}  // namespace sse_tick

#endif  // SSE_T0_TICK_ORDER_BOOK_H
