#include "sse/market_data/sse_tick_order_book.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct SlowOrder {
    std::uint64_t order_no;
    std::uint32_t price_raw;
    std::uint64_t quantity;
    char side;
    std::uint64_t add_time_micros;
};

class SlowBook {
public:
    SlowBook() : last_tick_index_(0) {}
    bool apply(const sse_live::TickEvent& event) {
        if (event.security_id != "600000" || !event.channel_no || !event.tick_index ||
            event.tick_index <= last_tick_index_) return false;
        last_tick_index_ = event.tick_index;
        if (event.event_type == 'A') {
            const char side = event.side == 0 ? 'B' : event.side == 1 ? 'S' : 0;
            const std::uint64_t order_no = side == 'B' ? event.buy_order_no : event.sell_order_no;
            if (!side || order_no == 0 || event.quantity_raw == 0 ||
                orders_.find(order_no) != orders_.end()) return false;
            orders_[order_no] = SlowOrder{order_no, event.price_raw,
                                          event.quantity_raw, side,
                                          event.time_of_day_micros};
            return true;
        }
        if (event.event_type == 'D') {
            const char side = event.side == 0 ? 'B' : event.side == 1 ? 'S' : 0;
            const std::uint64_t order_no = side == 'B' ? event.buy_order_no : event.sell_order_no;
            std::map<std::uint64_t, SlowOrder>::iterator it = orders_.find(order_no);
            if (!side || it == orders_.end()) return false;
            const std::uint64_t quantity = event.quantity_raw == 0
                ? it->second.quantity
                : std::min(event.quantity_raw, it->second.quantity);
            if (quantity == it->second.quantity) orders_.erase(it);
            else it->second.quantity -= quantity;
            return true;
        }
        if (event.event_type == 'T') {
            std::map<std::uint64_t, SlowOrder>::iterator buy = orders_.find(event.buy_order_no);
            bool accepted = false;
            if (buy != orders_.end()) {
                const std::uint64_t quantity = std::min(event.quantity_raw, buy->second.quantity);
                if (quantity) {
                    accepted = true;
                    if (quantity == buy->second.quantity) orders_.erase(buy);
                    else buy->second.quantity -= quantity;
                }
            }
            // Resolve the second side after the first mutation, including the
            // production behavior for a trade that repeats the same order ID.
            std::map<std::uint64_t, SlowOrder>::iterator sell = orders_.find(event.sell_order_no);
            if (sell != orders_.end()) {
                const std::uint64_t quantity = std::min(event.quantity_raw, sell->second.quantity);
                if (quantity) {
                    accepted = true;
                    if (quantity == sell->second.quantity) orders_.erase(sell);
                    else sell->second.quantity -= quantity;
                }
            }
            return accepted;
        }
        return event.event_type == 'S';
    }

    std::size_t live_order_count() const { return orders_.size(); }
    std::vector<SlowOrder> orders() const {
        std::vector<SlowOrder> result;
        for (std::map<std::uint64_t, SlowOrder>::const_iterator it = orders_.begin();
             it != orders_.end(); ++it) result.push_back(it->second);
        return result;
    }

    std::vector<sse_tick::Level> full_depth(char side,
                                            std::uint64_t now_micros) const {
        std::map<std::uint32_t, sse_tick::Level> values;
        for (std::map<std::uint64_t, SlowOrder>::const_iterator it = orders_.begin();
             it != orders_.end(); ++it) {
            const SlowOrder& order = it->second;
            if (order.side != side) continue;
            sse_tick::Level& level = values[order.price_raw];
            level.price_raw = static_cast<std::int64_t>(order.price_raw);
            level.quantity += order.quantity;
            level.order_count += 1;
            level.add_time_sum_micros += order.add_time_micros;
            if (now_micros >= order.add_time_micros &&
                now_micros - order.add_time_micros <= 30000000ULL)
                level.young_quantity += order.quantity;
        }
        std::vector<sse_tick::Level> result;
        if (side == 'B') {
            for (std::map<std::uint32_t, sse_tick::Level>::const_reverse_iterator it = values.rbegin();
                 it != values.rend(); ++it) result.push_back(it->second);
        } else {
            for (std::map<std::uint32_t, sse_tick::Level>::const_iterator it = values.begin();
                 it != values.end(); ++it) result.push_back(it->second);
        }
        return result;
    }

private:
    std::map<std::uint64_t, SlowOrder> orders_;
    std::uint64_t last_tick_index_;
};

std::uint64_t sequence = 0;

sse_live::TickEvent event(char type, char side, std::uint64_t buy,
                          std::uint64_t sell, std::uint32_t price,
                          std::uint64_t quantity, std::uint64_t time) {
    sse_live::TickEvent result = sse_live::TickEvent();
    result.security_id = "600000";
    result.channel_no = 1;
    result.provider_sequence = static_cast<std::uint32_t>(++sequence);
    result.tick_index = sequence;
    result.app_seq_num = sequence;
    result.time_of_day_micros = time;
    result.event_type = type;
    result.buy_order_no = buy;
    result.sell_order_no = sell;
    result.price_raw = price;
    result.quantity_raw = quantity;
    result.amount_raw = 0;
    result.side = side;
    return result;
}

bool same_level(const sse_tick::Level& left, const sse_tick::Level& right) {
    return left.price_raw == right.price_raw && left.quantity == right.quantity &&
           left.order_count == right.order_count &&
           left.add_time_sum_micros == right.add_time_sum_micros &&
           left.young_quantity == right.young_quantity;
}

bool compare_depth(sse_tick::OrderBook* fast, const SlowBook& slow,
                   std::uint64_t now, const char* label) {
    if (fast->live_order_count() != slow.live_order_count()) {
        std::cerr << label << ": live order count mismatch\n";
        return false;
    }
    std::vector<sse_tick::Level> bids;
    std::vector<sse_tick::Level> asks;
    if (!fast->full_depth('B', now, &bids) || !fast->full_depth('S', now, &asks)) {
        std::cerr << label << ": fast full_depth failed\n";
        return false;
    }
    const std::vector<sse_tick::Level> expected_bids = slow.full_depth('B', now);
    const std::vector<sse_tick::Level> expected_asks = slow.full_depth('S', now);
    if (bids.size() != expected_bids.size() || asks.size() != expected_asks.size()) {
        std::cerr << label << ": level count mismatch\n";
        return false;
    }
    for (std::size_t i = 0; i < bids.size(); ++i) {
        if (!same_level(bids[i], expected_bids[i])) {
            std::cerr << label << ": bid mismatch at " << i << "\n";
            return false;
        }
    }
    for (std::size_t i = 0; i < asks.size(); ++i) {
        if (!same_level(asks[i], expected_asks[i])) {
            std::cerr << label << ": ask mismatch at " << i << "\n";
            return false;
        }
    }
    sse_tick::Level snapshot_bids[8], snapshot_asks[8];
    if (!fast->snapshot(snapshot_bids, snapshot_asks, 8)) return false;
    for (std::size_t i = 0; i < 8; ++i) {
        sse_tick::Level bid = {0, 0, 0, 0, 0}, ask = {0, 0, 0, 0, 0};
        if (i < expected_bids.size()) bid = expected_bids[i];
        if (i < expected_asks.size()) ask = expected_asks[i];
        bid.add_time_sum_micros = bid.young_quantity = 0;
        ask.add_time_sum_micros = ask.young_quantity = 0;
        if (!same_level(snapshot_bids[i], bid) || !same_level(snapshot_asks[i], ask)) {
            std::cerr << label << ": snapshot mismatch at " << i << "\n";
            return false;
        }
    }
    return true;
}

bool apply_both(sse_tick::OrderBook* fast, SlowBook* slow,
                const sse_live::TickEvent& tick, const char* label);

bool edge_cases() {
    sse_tick::OrderBook fast("600000");
    SlowBook slow;
    const std::uint64_t t0 = 36000000123ULL;
    if (!compare_depth(&fast, slow, 0, "empty book at zero") ||
        !apply_both(&fast, &slow, event('A', 0, 301, 0, 10000, 100, t0), "edge old bid") ||
        !apply_both(&fast, &slow, event('A', 0, 302, 0, 10000, 200, t0 + 29999999), "edge young same price") ||
        !apply_both(&fast, &slow, event('A', 1, 0, 401, 10010, 300, t0 + 1), "edge ask") ||
        !compare_depth(&fast, slow, t0 - 1, "all orders future") ||
        !compare_depth(&fast, slow, t0, "exact add microsecond") ||
        !compare_depth(&fast, slow, t0 + 29999999, "age 29999999") ||
        !compare_depth(&fast, slow, t0 + 30000000, "age 30000000 inclusive") ||
        !compare_depth(&fast, slow, t0 + 30000001, "age 30000001 exclusive") ||
        !apply_both(&fast, &slow, event('D', 0, 301, 0, 0, 50, t0 + 30000002), "expired partial cancel") ||
        !compare_depth(&fast, slow, t0 + 30000002, "old cancel preserves young same price") ||
        !apply_both(&fast, &slow, event('T', 0, 301, 0, 10000, 10, t0 + 30000003), "expired partial fill") ||
        !compare_depth(&fast, slow, t0 + 30000003, "old fill preserves young same price") ||
        !apply_both(&fast, &slow, event('A', 1, 0, 301, 9900, 123, t0 + 30000004), "live duplicate ID rejected") ||
        !compare_depth(&fast, slow, t0 + 30000004, "duplicate leaves aggregates unchanged") ||
        !apply_both(&fast, &slow, event('D', 0, 301, 0, 0, 0, t0 + 30000005), "remove old ID") ||
        !apply_both(&fast, &slow, event('A', 1, 0, 301, 10020, 456, t0), "reuse removed ID different side") ||
        !compare_depth(&fast, slow, t0 + 30000006, "reused old ID expired") ||
        !compare_depth(&fast, slow, t0 + 1000, "rollback rehydrates only eligible orders") ||
        !apply_both(&fast, &slow, event('D', 0, 302, 0, 0, 50, t0 + 1001), "future partial cancel") ||
        !compare_depth(&fast, slow, t0 + 1001, "future cancel stays excluded") ||
        !compare_depth(&fast, slow, t0 + 29999999, "future order enters remaining quantity") ||
        !compare_depth(&fast, slow, t0 + 100000000, "expire every order") ||
        !compare_depth(&fast, slow, t0 + 2000, "second rollback")) return false;

    const std::vector<SlowOrder> pending = slow.orders();
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const SlowOrder& order = pending[i];
        if (!apply_both(&fast, &slow,
                event('D', order.side == 'B' ? 0 : 1,
                      order.side == 'B' ? order.order_no : 0,
                      order.side == 'S' ? order.order_no : 0,
                      0, 0, t0 + 100000001 + i), "clear remaining order")) return false;
    }
    if (!compare_depth(&fast, slow, t0 + 100000010, "all levels removed") ||
        !compare_depth(&fast, slow, 0, "cleared book rollback") ||
        !apply_both(&fast, &slow, event('A', 0, 301, 0, 10000, 999, 0), "reuse ID at time zero") ||
        !compare_depth(&fast, slow, 0, "zero time is young") ||
        !compare_depth(&fast, slow, 30000000, "zero time boundary") ||
        !compare_depth(&fast, slow, 30000001, "zero time expired") ||
        !compare_depth(&fast, slow, 0, "zero time rollback")) return false;

    // Sequence rejection must not mutate the book or its caches.
    sse_live::TickEvent repeat = event('A', 0, 999, 0, 9999, 1, t0);
    if (!apply_both(&fast, &slow, repeat, "first sequence accepted") ||
        !apply_both(&fast, &slow, repeat, "same sequence rejected") ||
        !compare_depth(&fast, slow, t0, "sequence rejection leaves cache unchanged")) return false;
    sse_tick::OrderBook fresh("600000");
    fast = fresh;
    slow = SlowBook();
    return compare_depth(&fast, slow, t0, "replacement clears all state");
}

std::uint64_t random_state = 0x617061746879ULL;
std::uint64_t random_next() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

bool randomized_reference() {
    sse_tick::OrderBook fast("600000");
    SlowBook slow;
    const std::uint64_t t0 = 34200000123ULL;
    for (std::uint64_t i = 0; i < 40000; ++i) {
        const std::uint64_t serial_time = t0 + i * 7001;
        // Out-of-order adds, future orders, reused live IDs, partial/complete
        // cancellation, unmatched trades, and same-ID two-sided trades.
        const std::uint64_t time = serial_time + random_next() % 70000001;
        const unsigned kind = static_cast<unsigned>(random_next() % 10);
        const char side = static_cast<char>(random_next() & 1);
        const std::uint64_t id = 1 + random_next() % 513;
        const std::uint64_t other = random_next() % 7 == 0 ? id : 1 + random_next() % 513;
        const std::uint32_t price = 9800 + static_cast<std::uint32_t>(random_next() % 41) * 10;
        const std::uint64_t qty = random_next() % 5 == 0 ? 0 : 1 + random_next() % 1000;
        const char type = kind < 4 ? 'A' : kind < 7 ? 'D' : kind < 9 ? 'T' : 'S';
        if (!apply_both(&fast, &slow,
                event(type, side, type == 'T' || side == 0 ? id : 0,
                      type == 'T' ? other : side == 1 ? id : 0,
                      price, qty, time), "random apply")) return false;
        std::uint64_t now = serial_time + random_next() % 90000001;
        if (i % 17 == 0) now = t0 + random_next() % 1000000;
        if (!compare_depth(&fast, slow, now, "random query")) {
            std::cerr << "random iteration " << i << "\n";
            return false;
        }
        if (i % 61 == 0) {
            const sse_tick::FlowStats window = fast.take_flow_window();
            (void)window;
            if (!compare_depth(&fast, slow, now, "random flow clear")) return false;
        }
    }
    return true;
}

bool flow_semantics() {
    sse_tick::OrderBook book("600000");
    if (!book.apply(event('A', 0, 501, 0, 10000, 1000, 123)).accepted ||
        !book.apply(event('A', 1, 0, 601, 10000, 3000, 124)).accepted) return false;
    const sse_tick::FlowStats adds = book.take_flow_window();
    if (adds.buy_add_qty != 1000 || adds.sell_add_qty != 3000 || adds.events.size() != 2) return false;
    if (!book.apply(event('T', 0, 501, 601, 10000, 250, 125)).accepted) return false;
    const sse_tick::FlowStats trade = book.take_flow_window();
    if (trade.buy_trade_qty != 250 || trade.sell_trade_qty != 250 ||
        trade.trade_count != 1 || trade.trade_turnover != 2.5 ||
        trade.events.size() != 1 || trade.events[0].quantity != 250 ||
        book.total_trade_qty() != 250 || book.total_trade_turnover() != 2.5 ||
        book.last_trade_price() != 10.0) return false;
    // Unmatched T still contributes turnover, matching today's production
    // semantics, but does not add matched volume or a flow event.
    if (book.apply(event('T', 0, 999, 998, 11000, 1000, 126)).accepted) return false;
    const sse_tick::FlowStats unmatched = book.take_flow_window();
    if (unmatched.trade_count || !unmatched.events.empty() ||
        unmatched.trade_turnover != 11.0 || book.total_trade_qty() != 250 ||
        book.total_trade_turnover() != 13.5 || book.last_trade_price() != 10.0) return false;
    return true;
}

bool flow_buffer_reuse() {
    sse_tick::OrderBook book("600000");
    sse_tick::FlowStats window;
    window.events.reserve(512);
    for (std::uint64_t i = 0; i < 512; ++i) {
        if (!book.apply(event('A', 0, 10000 + i, 0, 10000,
                              1, 1000 + i)).accepted) return false;
    }
    book.take_flow_window(window);
    if (window.events.size() != 512 || window.events.capacity() < 512) {
        std::cerr << "flow reuse setup failed\n";
        return false;
    }
    const std::size_t capacity = window.events.capacity();
    if (!book.apply(event('A', 0, 20000, 0, 10010, 1, 2000)).accepted) return false;
    book.take_flow_window(window);
    // Both sides now have the high-water capacity. A later sample must reuse
    // it even though the output ownership changes hands on each take.
    if (window.events.size() != 1 || window.events.capacity() != capacity) {
        std::cerr << "flow output capacity was not recycled\n";
        return false;
    }
    book.take_flow_window(window);
    return window.events.empty() && window.events.capacity() == capacity;
}

bool apply_both(sse_tick::OrderBook* fast, SlowBook* slow,
                const sse_live::TickEvent& tick, const char* label) {
    const sse_tick::ApplyResult result = fast->apply(tick);
    const bool expected = slow->apply(tick);
    if (result.accepted != expected) {
        std::cerr << label << ": accepted mismatch\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    sse_tick::OrderBook fast("600000");
    SlowBook slow;
    const std::uint64_t t0 = 36000000000ULL;

    if (!apply_both(&fast, &slow, event('A', 0, 101, 0, 10000, 1000, t0), "add bid 1") ||
        !apply_both(&fast, &slow, event('A', 0, 102, 0, 10000, 2000, t0 + 1000), "add bid 2") ||
        !apply_both(&fast, &slow, event('A', 0, 103, 0, 9990, 500, t0 + 2000), "add bid 3") ||
        !apply_both(&fast, &slow, event('A', 1, 0, 201, 10010, 1200, t0), "add ask 1") ||
        !apply_both(&fast, &slow, event('A', 1, 0, 202, 10020, 700, t0 + 1000), "add ask 2") ||
        !compare_depth(&fast, slow, t0 + 1000, "initial depth")) return 1;

    // The exact 30-second boundary is young; one microsecond later the oldest
    // order is expired while newer orders can remain young.
    if (!compare_depth(&fast, slow, t0 + 30000000ULL, "young at 30 seconds") ||
        !compare_depth(&fast, slow, t0 + 30000001ULL, "young at 30 seconds plus one")) return 1;

    if (!apply_both(&fast, &slow, event('D', 0, 101, 0, 0, 400, t0 + 30000002ULL), "partial cancel") ||
        !compare_depth(&fast, slow, t0 + 30000002ULL, "after partial cancel") ||
        !apply_both(&fast, &slow, event('T', 0, 102, 201, 10005, 1000, t0 + 30000003ULL), "partial trade") ||
        !compare_depth(&fast, slow, t0 + 30000003ULL, "after partial trade") ||
        !apply_both(&fast, &slow, event('D', 0, 101, 0, 0, 0, t0 + 30000004ULL), "last order removal") ||
        !compare_depth(&fast, slow, t0 + 30000004ULL, "after last order removal")) return 1;

    // Querying before an order's exchange timestamp excludes it, and a clock
    // rollback rehydrates entries that were previously outside the age window.
    if (!apply_both(&fast, &slow, event('A', 0, 104, 0, 9980, 900, t0 + 40000000ULL), "future-timestamp add") ||
        !compare_depth(&fast, slow, t0 + 39999999ULL, "before future timestamp") ||
        !compare_depth(&fast, slow, t0 + 70000000ULL, "future timestamp at boundary") ||
        !compare_depth(&fast, slow, t0 + 70000001ULL, "future timestamp expired") ||
        !compare_depth(&fast, slow, t0 + 40000000ULL, "clock rollback")) return 1;
    // Removing an order outside the current young window must update the
    // aggregate book without requiring an active-young tree entry.
    if (!apply_both(&fast, &slow, event('A', 0, 105, 0, 9970, 777, t0), "old order add") ||
        !compare_depth(&fast, slow, t0 + 40000000ULL, "old order not young") ||
        !apply_both(&fast, &slow, event('D', 0, 105, 0, 0, 0, t0 + 40000001ULL), "old order removal") ||
        !compare_depth(&fast, slow, t0 + 40000001ULL, "after old order removal")) return 1;

    // The return-by-value flow API remains available for callers that need
    // ownership of a single window.
    const sse_tick::FlowStats window = fast.take_flow_window();
    if (window.events.empty()) {
        std::cerr << "flow window consumption failed\n";
        return 1;
    }
    const sse_tick::FlowStats empty = fast.take_flow_window();
    if (!empty.events.empty() || empty.buy_add_qty || empty.sell_add_qty ||
        empty.buy_cancel_qty || empty.sell_cancel_qty || empty.buy_trade_qty ||
        empty.sell_trade_qty || empty.positive_trade_qty || empty.negative_trade_qty ||
        empty.trade_count || empty.trade_turnover != 0.0) {
        std::cerr << "flow window clear failed\n";
        return 1;
    }
    if (!compare_depth(&fast, slow, t0 + 40000001ULL, "flow clear preserves book")) return 1;
    if (!edge_cases() || !randomized_reference() || !flow_semantics() ||
        !flow_buffer_reuse()) return 1;
    std::cout << "sse_book_incremental_test: PASS (directed + 40000 reference events)\n";
    return 0;
}
