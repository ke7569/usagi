#include "sse/market_data/sse_tick_order_book.h"

#include <algorithm>
#include <cmath>

namespace sse_tick {
namespace {

char sse_side_for_order(const sse_live::TickEvent& event) {
    // EFH tick_merge m_side_flag: 0=buy, 1=sell for A/D. T uses both
    // explicit order numbers and never uses this flag as a direction.
    if (event.side == 0) return 'B';
    if (event.side == 1) return 'S';
    return 0;
}

}

FlowStats::FlowStats()
    : buy_add_qty(0), sell_add_qty(0), buy_cancel_qty(0), sell_cancel_qty(0),
      buy_trade_qty(0), sell_trade_qty(0), positive_trade_qty(0),
      negative_trade_qty(0), trade_count(0), trade_turnover(0.0), events() {}

void FlowStats::clear_window() {
    buy_add_qty = sell_add_qty = buy_cancel_qty = sell_cancel_qty = 0;
    buy_trade_qty = sell_trade_qty = positive_trade_qty = negative_trade_qty = 0;
    trade_count = 0;
    trade_turnover = 0.0;
    events.clear();
}

ApplyResult::ApplyResult()
    : accepted(false), state_event(false), book_changed(false),
      sequence_healthy(true), reason("uninitialized") {}

OrderBook::OrderBook(const std::string& security_id)
    : security_id_(security_id), orders_(), bid_qty_(), ask_qty_(), bid_count_(),
      ask_count_(), flow_(), last_tick_index_(0), has_tick_index_(false),
      total_trade_qty_(0), total_trade_turnover_(0.0), last_trade_price_(0.0) {
    flow_.events.reserve(256);
}

OrderBook::QuantityMap& OrderBook::quantities(char side) {
    return side == 'B' ? bid_qty_ : ask_qty_;
}

const OrderBook::QuantityMap& OrderBook::quantities(char side) const {
    return side == 'B' ? bid_qty_ : ask_qty_;
}

OrderBook::CountMap& OrderBook::counts(char side) {
    return side == 'B' ? bid_count_ : ask_count_;
}

const OrderBook::CountMap& OrderBook::counts(char side) const {
    return side == 'B' ? bid_count_ : ask_count_;
}

void OrderBook::add_level(std::uint32_t price, std::uint64_t quantity,
                          char side) {
    quantities(side)[price] += quantity;
    counts(side)[price] += 1;
}

void OrderBook::remove_level(std::uint32_t price, std::uint64_t quantity,
                             char side) {
    QuantityMap& q = quantities(side);
    QuantityMap::iterator qi = q.find(price);
    if (qi != q.end()) {
        qi->second = qi->second > quantity ? qi->second - quantity : 0;
        if (qi->second == 0) q.erase(qi);
    }
    // The caller removes the order from orders_ separately. Count is adjusted
    // there, so partial fills/cancels do not accidentally drop a level count.
}

bool OrderBook::add_order(std::uint64_t order_no, std::uint32_t price,
                          std::uint64_t quantity, char side,
                          std::uint64_t tick, std::uint64_t time_micros) {
    if (order_no == 0 || quantity == 0 || (side != 'B' && side != 'S')) return false;
    if (orders_.find(order_no) != orders_.end()) return false;
    Order order = {order_no, price, quantity, side, tick, time_micros};
    orders_[order_no] = order;
    add_level(price, quantity, side);
    if (side == 'B') flow_.buy_add_qty += quantity;
    else flow_.sell_add_qty += quantity;
    flow_.events.push_back(FlowEvent{'A', side, order_no, 0, price, quantity});
    return true;
}

bool OrderBook::delete_order(std::uint64_t order_no,
                             std::uint64_t quantity_hint,
                             std::uint32_t event_price_raw) {
    std::unordered_map<std::uint64_t, Order>::iterator it = orders_.find(order_no);
    if (it == orders_.end()) return false;
    const Order order = it->second;
    const std::uint64_t quantity = quantity_hint == 0 ? order.remaining_qty
                                  : std::min(quantity_hint, order.remaining_qty);
    remove_level(order.price_raw, quantity, order.side);
    if (order.side == 'B') flow_.buy_cancel_qty += quantity;
    else flow_.sell_cancel_qty += quantity;
    flow_.events.push_back(FlowEvent{'D', order.side, order_no, 0,
                                     order.price_raw ? order.price_raw : event_price_raw,
                                     quantity});
    if (quantity >= order.remaining_qty) {
        CountMap& c = counts(order.side);
        CountMap::iterator ci = c.find(order.price_raw);
        if (ci != c.end()) {
            if (ci->second > 1) --ci->second;
            else c.erase(ci);
        }
        orders_.erase(it);
    } else {
        it->second.remaining_qty -= quantity;
    }
    return true;
}

bool OrderBook::trade_order(std::uint64_t order_no, std::uint64_t quantity,
                            std::uint64_t* applied) {
    std::unordered_map<std::uint64_t, Order>::iterator it = orders_.find(order_no);
    if (it == orders_.end()) return false;
    const Order order = it->second;
    const std::uint64_t used = std::min(quantity, order.remaining_qty);
    remove_level(order.price_raw, used, order.side);
    if (order.side == 'B') flow_.buy_trade_qty += used;
    else flow_.sell_trade_qty += used;
    if (applied) *applied = used;
    if (used >= order.remaining_qty) {
        CountMap& c = counts(order.side);
        CountMap::iterator ci = c.find(order.price_raw);
        if (ci != c.end()) {
            if (ci->second > 1) --ci->second;
            else c.erase(ci);
        }
        orders_.erase(it);
    } else {
        it->second.remaining_qty -= used;
    }
    return used != 0;
}

FlowStats OrderBook::take_flow_window() {
    FlowStats out;
    out.buy_add_qty = flow_.buy_add_qty;
    out.sell_add_qty = flow_.sell_add_qty;
    out.buy_cancel_qty = flow_.buy_cancel_qty;
    out.sell_cancel_qty = flow_.sell_cancel_qty;
    out.buy_trade_qty = flow_.buy_trade_qty;
    out.sell_trade_qty = flow_.sell_trade_qty;
    out.positive_trade_qty = flow_.positive_trade_qty;
    out.negative_trade_qty = flow_.negative_trade_qty;
    out.trade_count = flow_.trade_count;
    out.trade_turnover = flow_.trade_turnover;
    out.events.swap(flow_.events);
    flow_.clear_window();
    flow_.events.reserve(256);
    return out;
}

ApplyResult OrderBook::apply(const sse_live::TickEvent& event) {
    ApplyResult result;
    if (event.security_id != security_id_) { result.reason = "security_mismatch"; return result; }
    if (event.channel_no == 0 || event.tick_index == 0) { result.reason = "invalid_channel_or_tick"; return result; }
    if (has_tick_index_ && event.tick_index <= last_tick_index_) {
        result.sequence_healthy = false;
        result.reason = "non_monotonic_tick_index";
        return result;
    }
    has_tick_index_ = true;
    last_tick_index_ = event.tick_index;
    if (event.event_type == 'S') {
        result.accepted = true; result.state_event = true; result.reason = "status"; return result;
    }
    if (event.event_type == 'A') {
        const char side = sse_side_for_order(event);
        const std::uint64_t order_no = side == 'B' ? event.buy_order_no : event.sell_order_no;
        const bool ok = add_order(order_no, event.price_raw, event.quantity_raw, side,
                                  event.tick_index, event.time_of_day_micros);
        result.accepted = ok; result.book_changed = ok;
        result.reason = ok ? "add" : "invalid_add";
        return result;
    }
    if (event.event_type == 'D') {
        const char side = sse_side_for_order(event);
        const std::uint64_t order_no = side == 'B' ? event.buy_order_no : event.sell_order_no;
        const bool ok = delete_order(order_no, event.quantity_raw, event.price_raw);
        result.accepted = ok; result.book_changed = ok;
        result.reason = ok ? "delete" : "unknown_delete";
        return result;
    }
    if (event.event_type == 'T') {
        // Sampling eligibility follows the exchange's cumulative turnover,
        // which is carried by every T message even when an order-id lookup
        // cannot update our reconstructed book.  Record that amount before
        // attempting the two order-side matches.
        const double event_price = static_cast<double>(event.price_raw) / 1000.0;
        const double event_turnover = event_price * static_cast<double>(event.quantity_raw) / 1000.0;
        if (event.quantity_raw != 0U && event_price > 0.0) {
            total_trade_turnover_ += event_turnover;
            flow_.trade_turnover += event_turnover;
        }
        std::uint64_t buy_used = 0, sell_used = 0;
        const bool buy_ok = trade_order(event.buy_order_no, event.quantity_raw, &buy_used);
        const bool sell_ok = trade_order(event.sell_order_no, event.quantity_raw, &sell_used);
        const std::uint64_t applied = std::max(buy_used, sell_used);
        if (applied != 0) {
            ++flow_.trade_count;
            total_trade_qty_ += applied;
            last_trade_price_ = event_price;
            if (event.buy_order_no > event.sell_order_no) ++flow_.positive_trade_qty;
            else if (event.buy_order_no < event.sell_order_no) ++flow_.negative_trade_qty;
            // Store the raw order ids so the factor layer can reproduce the
            // SSE direction and weighted order-flow terms exactly.
            flow_.events.push_back(FlowEvent{'T', 0, event.buy_order_no,
                                             event.sell_order_no, event.price_raw,
                                             applied});
        }
        result.accepted = buy_ok || sell_ok; result.book_changed = result.accepted;
        result.reason = result.accepted ? "trade" : "unknown_trade";
        return result;
    }
    result.reason = "unknown_event";
    return result;
}

bool OrderBook::snapshot(Level* bids, Level* asks, std::size_t depth) const {
    if (!bids || !asks) return false;
    for (std::size_t i = 0; i < depth; ++i) {
        bids[i] = Level{0, 0, 0, 0, 0};
        asks[i] = Level{0, 0, 0, 0, 0};
    }
    std::size_t bi = 0;
    for (QuantityMap::const_reverse_iterator it = bid_qty_.rbegin();
         it != bid_qty_.rend() && bi < depth; ++it, ++bi) {
        const CountMap::const_iterator ci = bid_count_.find(it->first);
        bids[bi] = Level{static_cast<std::int64_t>(it->first), it->second,
                         ci == bid_count_.end() ? 0 : ci->second, 0, 0};
    }
    std::size_t ai = 0;
    for (QuantityMap::const_iterator it = ask_qty_.begin();
         it != ask_qty_.end() && ai < depth; ++it, ++ai) {
        const CountMap::const_iterator ci = ask_count_.find(it->first);
        asks[ai] = Level{static_cast<std::int64_t>(it->first), it->second,
                         ci == ask_count_.end() ? 0 : ci->second, 0, 0};
    }
    return true;
}

bool OrderBook::full_depth(char side, std::uint64_t now_micros,
                           std::vector<Level>* levels) const {
    if (!levels || (side != 'B' && side != 'S')) return false;
    levels->clear();
    const QuantityMap& q = quantities(side);
    const CountMap& c = counts(side);
    levels->reserve(q.size());
    const bool buy = side == 'B';
    std::map<std::uint32_t, std::size_t> level_index;
    if (buy) {
        for (QuantityMap::const_reverse_iterator it = q.rbegin(); it != q.rend(); ++it) {
            const CountMap::const_iterator ci = c.find(it->first);
            Level value{static_cast<std::int64_t>(it->first), it->second,
                        ci == c.end() ? 0 : ci->second, 0, 0};
            level_index[it->first] = levels->size();
            levels->push_back(value);
        }
    } else {
        for (QuantityMap::const_iterator it = q.begin(); it != q.end(); ++it) {
            const CountMap::const_iterator ci = c.find(it->first);
            Level value{static_cast<std::int64_t>(it->first), it->second,
                        ci == c.end() ? 0 : ci->second, 0, 0};
            level_index[it->first] = levels->size();
            levels->push_back(value);
        }
    }
    // Aggregate order ages once per order. The old implementation nested this
    // walk under every price level, which made a full-day replay quadratic in
    // the number of resting levels and orders.
    for (std::unordered_map<std::uint64_t, Order>::const_iterator oi = orders_.begin();
         oi != orders_.end(); ++oi) {
        if (oi->second.side != side) continue;
        std::map<std::uint32_t, std::size_t>::const_iterator li =
            level_index.find(oi->second.price_raw);
        if (li == level_index.end()) continue;
        Level& level = (*levels)[li->second];
        level.add_time_sum_micros += oi->second.add_time_micros;
        if (now_micros >= oi->second.add_time_micros &&
            now_micros - oi->second.add_time_micros <= 30000000ULL)
            level.young_quantity += oi->second.remaining_qty;
    }
    return true;
}

}  // namespace sse_tick
