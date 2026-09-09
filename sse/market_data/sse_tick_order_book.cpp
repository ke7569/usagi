#include "sse/market_data/sse_tick_order_book.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sse_tick {
namespace {

static const std::uint32_t kYoungWindowSeconds = 30U;

template <typename Event>
char sse_side_for_order(const Event& event) {
    // EFH tick_merge m_side_flag: 0=buy, 1=sell for A/D. T uses both
    // explicit order numbers and never uses this flag as a direction.
    if (event.side == 0) return 'B';
    if (event.side == 1) return 'S';
    return 0;
}

bool security_matches(const std::string& security_id,
                      const sse_live::TickEvent& event) {
    return event.security_id == security_id;
}

bool security_matches(const std::string& security_id,
                      const sse_live::DecodedTick& event) {
    std::size_t encoded_size = 0;
    while (encoded_size < sizeof(event.security_id) &&
           event.security_id[encoded_size] != '\0')
        ++encoded_size;
    if (encoded_size == sizeof(event.security_id) ||
        encoded_size != security_id.size()) return false;
    for (std::size_t i = 0; i < security_id.size(); ++i)
        if (event.security_id[i] != security_id[i]) return false;
    return true;
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
    : security_id_(security_id), orders_(), bid_levels_(), ask_levels_(),
      age_entries_(), age_activate_index_(0), age_expire_index_(0),
      next_age_generation_(0),
      young_cache_initialized_(false), young_cache_time_seconds_(0), flow_(), last_tick_index_(0),
      has_tick_index_(false), total_trade_qty_(0), total_trade_turnover_(0.0),
      last_trade_price_(0.0) {
    flow_.events.reserve(256);
    age_entries_.reserve(4096);
}

OrderBook::LevelMap& OrderBook::levels(char side) {
    return side == 'B' ? bid_levels_ : ask_levels_;
}

const OrderBook::LevelMap& OrderBook::levels(char side) const {
    return side == 'B' ? bid_levels_ : ask_levels_;
}

bool OrderBook::is_young(std::uint32_t now_seconds,
                         std::uint32_t add_time_seconds) {
    return now_seconds >= add_time_seconds &&
           now_seconds - add_time_seconds <= kYoungWindowSeconds;
}

bool OrderBook::is_expired(std::uint32_t now_seconds,
                           std::uint32_t add_time_seconds) {
    // The 30-second boundary remains young. An order expires only once its
    // age is strictly greater than the configured window.
    return now_seconds > kYoungWindowSeconds &&
           add_time_seconds < now_seconds - kYoungWindowSeconds;
}

void OrderBook::refresh_young(std::uint64_t now_micros) const {
    const std::uint32_t now_seconds =
        static_cast<std::uint32_t>(now_micros / 1000000ULL);
    if (!young_cache_initialized_) {
        rebuild_young(now_micros);
        return;
    }
    if (now_seconds < young_cache_time_seconds_) {
        // A replay/query clock can move backwards. Rebuild from the compact
        // live order table; this is the exceptional recovery path.
        rebuild_young(now_micros);
        return;
    }
    if (now_seconds == young_cache_time_seconds_) return;

    const std::uint32_t old_seconds = young_cache_time_seconds_;
    while (age_activate_index_ < age_entries_.size() &&
           age_entries_[age_activate_index_].add_time_seconds <= now_seconds) {
        const AgeEntry& entry = age_entries_[age_activate_index_++];
        if (entry.add_time_seconds <= old_seconds ||
            !is_young(now_seconds, entry.add_time_seconds)) continue;
        const std::unordered_map<std::uint64_t, Order>::const_iterator oi =
            orders_.find(entry.order_no);
        if (oi == orders_.end()) continue;
        if (oi->second.add_time_seconds != entry.add_time_seconds ||
            oi->second.age_generation != entry.age_generation) continue;
        const LevelMap& side_levels = levels(oi->second.side);
        LevelMap::const_iterator li = side_levels.find(oi->second.price_raw);
        if (li == side_levels.end()) continue;
        li->second.young_quantity += oi->second.remaining_qty;
    }

    const std::uint32_t cutoff = is_expired(now_seconds, 0U)
        ? now_seconds - kYoungWindowSeconds : 0U;
    while (age_expire_index_ < age_activate_index_ &&
           age_entries_[age_expire_index_].add_time_seconds < cutoff) {
        const AgeEntry& entry = age_entries_[age_expire_index_++];
        // Entries added after the previous query were not counted in the
        // young aggregate yet, so they must not be subtracted here.
        if (entry.add_time_seconds > old_seconds) continue;
        const std::unordered_map<std::uint64_t, Order>::const_iterator oi =
            orders_.find(entry.order_no);
        if (oi != orders_.end() &&
            oi->second.add_time_seconds == entry.add_time_seconds &&
            oi->second.age_generation == entry.age_generation) {
            const LevelMap& side_levels = levels(oi->second.side);
            LevelMap::const_iterator li = side_levels.find(oi->second.price_raw);
            if (li != side_levels.end()) {
                li->second.young_quantity =
                    li->second.young_quantity > oi->second.remaining_qty
                        ? li->second.young_quantity - oi->second.remaining_qty
                        : 0ULL;
            }
        }
    }
    young_cache_time_seconds_ = now_seconds;
    compact_age_entries();
}

void OrderBook::rebuild_young(std::uint64_t now_micros) const {
    const std::uint32_t now_seconds =
        static_cast<std::uint32_t>(now_micros / 1000000ULL);
    for (LevelMap::const_iterator it = bid_levels_.begin();
         it != bid_levels_.end(); ++it)
        it->second.young_quantity = 0;
    for (LevelMap::const_iterator it = ask_levels_.begin();
         it != ask_levels_.end(); ++it)
        it->second.young_quantity = 0;
    for (std::unordered_map<std::uint64_t, Order>::const_iterator oi = orders_.begin();
         oi != orders_.end(); ++oi) {
        if (!is_young(now_seconds, oi->second.add_time_seconds)) continue;
        const LevelMap& side_levels = levels(oi->second.side);
        LevelMap::const_iterator li = side_levels.find(oi->second.price_raw);
        if (li == side_levels.end()) continue;
        li->second.young_quantity += oi->second.remaining_qty;
    }
    age_activate_index_ = 0;
    while (age_activate_index_ < age_entries_.size() &&
           age_entries_[age_activate_index_].add_time_seconds <= now_seconds)
        ++age_activate_index_;
    const std::uint32_t cutoff = is_expired(now_seconds, 0U)
        ? now_seconds - kYoungWindowSeconds : 0U;
    age_expire_index_ = 0;
    while (age_expire_index_ < age_activate_index_ &&
           age_entries_[age_expire_index_].add_time_seconds < cutoff)
        ++age_expire_index_;
    young_cache_time_seconds_ = now_seconds;
    young_cache_initialized_ = true;
    compact_age_entries();
}

void OrderBook::compact_age_entries() const {
    if (age_expire_index_ < 65536U ||
        age_expire_index_ * 2U <= age_entries_.size()) return;
    const std::size_t removed = age_expire_index_;
    age_entries_.erase(age_entries_.begin(), age_entries_.begin() + removed);
    age_activate_index_ -= removed;
    age_expire_index_ = 0;
}

bool OrderBook::add_order(std::uint64_t order_no, std::uint32_t price,
                          std::uint64_t quantity, char side,
                          std::uint64_t tick, std::uint64_t time_micros) {
    if (order_no == 0 || quantity == 0 || (side != 'B' && side != 'S')) return false;
    if (orders_.find(order_no) != orders_.end()) return false;
    (void)tick;
    const std::uint32_t add_time_seconds =
        static_cast<std::uint32_t>(time_micros / 1000000ULL);
    if (++next_age_generation_ == 0U) ++next_age_generation_;
    Order order = {order_no, price, quantity, side, add_time_seconds,
                   next_age_generation_};
    orders_[order_no] = order;
    LevelAggregate& level = levels(side)[price];
    level.quantity += quantity;
    level.order_count += 1;
    level.add_time_sum_micros += static_cast<std::uint64_t>(add_time_seconds) * 1000000ULL;
    age_entries_.push_back(AgeEntry{add_time_seconds, next_age_generation_, order_no});
    if (!young_cache_initialized_) {
        young_cache_initialized_ = true;
        young_cache_time_seconds_ = add_time_seconds;
    }
    if (is_young(young_cache_time_seconds_, add_time_seconds)) {
        level.young_quantity += quantity;
        // The entry is already reflected in the aggregate. Mark it as
        // activated so the next forward refresh does not add it twice.
        age_activate_index_ = age_entries_.size();
    }
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
    const bool remove_order = quantity >= order.remaining_qty;
    remove_order_quantity(order, quantity, remove_order);
    if (order.side == 'B') flow_.buy_cancel_qty += quantity;
    else flow_.sell_cancel_qty += quantity;
    flow_.events.push_back(FlowEvent{'D', order.side, order_no, 0,
                                     order.price_raw ? order.price_raw : event_price_raw,
                                     quantity});
    if (remove_order) {
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
    const bool remove_order = used >= order.remaining_qty;
    remove_order_quantity(order, used, remove_order);
    if (order.side == 'B') flow_.buy_trade_qty += used;
    else flow_.sell_trade_qty += used;
    if (applied) *applied = used;
    if (remove_order) {
        orders_.erase(it);
    } else {
        it->second.remaining_qty -= used;
    }
    return used != 0;
}

void OrderBook::remove_order_quantity(const Order& order,
                                      std::uint64_t quantity,
                                      bool remove_order) {
    LevelMap& side_levels = levels(order.side);
    LevelMap::iterator li = side_levels.find(order.price_raw);
    if (li != side_levels.end()) {
        LevelAggregate& level = li->second;
        level.quantity = level.quantity > quantity ? level.quantity - quantity : 0ULL;
        // The young aggregate is maintained as the cached live-order
        // predicate at young_cache_time_seconds_. Avoid a tree lookup on every
        // fill/cancel.
        const bool young = young_cache_initialized_ &&
                           is_young(young_cache_time_seconds_, order.add_time_seconds);
        if (young) {
            level.young_quantity = level.young_quantity > quantity
                ? level.young_quantity - quantity : 0ULL;
        }
        if (remove_order) {
            if (level.order_count > 0) --level.order_count;
            const std::uint64_t add_time_micros =
                static_cast<std::uint64_t>(order.add_time_seconds) * 1000000ULL;
            level.add_time_sum_micros =
                level.add_time_sum_micros > add_time_micros
                    ? level.add_time_sum_micros - add_time_micros : 0ULL;
            if (level.order_count == 0 || level.quantity == 0)
                side_levels.erase(li);
        }
    }
}

void OrderBook::take_flow_window(FlowStats* out) {
    if (!out) return;
    out->clear_window();
    out->buy_add_qty = flow_.buy_add_qty;
    out->sell_add_qty = flow_.sell_add_qty;
    out->buy_cancel_qty = flow_.buy_cancel_qty;
    out->sell_cancel_qty = flow_.sell_cancel_qty;
    out->buy_trade_qty = flow_.buy_trade_qty;
    out->sell_trade_qty = flow_.sell_trade_qty;
    out->positive_trade_qty = flow_.positive_trade_qty;
    out->negative_trade_qty = flow_.negative_trade_qty;
    out->trade_count = flow_.trade_count;
    out->trade_turnover = flow_.trade_turnover;
    out->events.swap(flow_.events);
    flow_.clear_window();
}

FlowStats OrderBook::take_flow_window() {
    FlowStats out;
    take_flow_window(&out);
    return out;
}

template <typename Event>
ApplyResult OrderBook::apply_impl(const Event& event) {
    ApplyResult result;
    if (!security_matches(security_id_, event)) {
        result.reason = "security_mismatch";
        return result;
    }
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

ApplyResult OrderBook::apply(const sse_live::TickEvent& event) {
    return apply_impl(event);
}

ApplyResult OrderBook::apply(const sse_live::DecodedTick& event) {
    return apply_impl(event);
}

bool OrderBook::snapshot(Level* bids, Level* asks, std::size_t depth) const {
    if (!bids || !asks) return false;
    for (std::size_t i = 0; i < depth; ++i) {
        bids[i] = Level{0, 0, 0, 0, 0};
        asks[i] = Level{0, 0, 0, 0, 0};
    }
    std::size_t bi = 0;
    for (LevelMap::const_reverse_iterator it = bid_levels_.rbegin();
         it != bid_levels_.rend() && bi < depth; ++it, ++bi) {
        bids[bi] = Level{static_cast<std::int64_t>(it->first),
                         it->second.quantity, it->second.order_count, 0, 0};
    }
    std::size_t ai = 0;
    for (LevelMap::const_iterator it = ask_levels_.begin();
         it != ask_levels_.end() && ai < depth; ++it, ++ai) {
        asks[ai] = Level{static_cast<std::int64_t>(it->first),
                         it->second.quantity, it->second.order_count, 0, 0};
    }
    return true;
}

bool OrderBook::full_depth(char side, std::uint64_t now_micros,
                           std::vector<Level>* levels) const {
    if (!levels || (side != 'B' && side != 'S')) return false;
    refresh_young(now_micros);
    levels->clear();
    const LevelMap& side_levels = this->levels(side);
    levels->reserve(side_levels.size());
    const bool buy = side == 'B';
    if (buy) {
        for (LevelMap::const_reverse_iterator it = side_levels.rbegin();
             it != side_levels.rend(); ++it) {
            levels->push_back(Level{
                static_cast<std::int64_t>(it->first), it->second.quantity,
                it->second.order_count, it->second.add_time_sum_micros,
                it->second.young_quantity});
        }
    } else {
        for (LevelMap::const_iterator it = side_levels.begin();
             it != side_levels.end(); ++it) {
            levels->push_back(Level{
                static_cast<std::int64_t>(it->first), it->second.quantity,
                it->second.order_count, it->second.add_time_sum_micros,
                it->second.young_quantity});
        }
    }
    return true;
}

}  // namespace sse_tick
