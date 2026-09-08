#include "sse/market_data/sse_decoded_tick.h"
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
    bool apply(const sse_live::TickEvent& event) {
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
            std::map<std::uint64_t, SlowOrder>::iterator sell = orders_.find(event.sell_order_no);
            bool accepted = false;
            if (buy != orders_.end()) {
                const std::uint64_t quantity = std::min(event.quantity_raw, buy->second.quantity);
                if (quantity) {
                    accepted = true;
                    if (quantity == buy->second.quantity) orders_.erase(buy);
                    else buy->second.quantity -= quantity;
                }
            }
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
};

std::uint64_t sequence = 0;

sse_live::TickEvent event(char type, char side, std::uint64_t buy,
                          std::uint64_t sell, std::uint32_t price,
                          std::uint64_t quantity, std::uint64_t time) {
    sse_live::TickEvent result;
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
    return true;
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
        !apply_both(&fast, &slow, event('A', 1, 201, 0, 10010, 1200, t0), "add ask 1") ||
        !apply_both(&fast, &slow, event('A', 1, 202, 0, 10020, 700, t0 + 1000), "add ask 2") ||
        !compare_depth(&fast, slow, t0 + 1000, "initial depth")) return 1;

    // The exact 30-second boundary is still young; one microsecond later it is
    // still young until the age is strictly greater than 30 seconds.
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

    // A direct POD handoff follows the same path without constructing a
    // TickEvent string.
    sse_tick::OrderBook decoded_book("600000");
    sse_live::DecodedTick decoded;
    std::memset(&decoded, 0, sizeof(decoded));
    decoded.security_id[0] = '6'; decoded.security_id[1] = '0';
    decoded.security_id[2] = '0'; decoded.security_id[3] = '0';
    decoded.security_id[4] = '0'; decoded.security_id[5] = '0';
    decoded.channel_no = 1; decoded.provider_sequence = 1;
    decoded.tick_index = 1; decoded.app_seq_num = 1;
    decoded.time_of_day_micros = t0; decoded.event_type = 'A';
    decoded.buy_order_no = 9001; decoded.price_raw = 10000;
    decoded.quantity_raw = 321; decoded.side = 0;
    if (!decoded_book.apply(decoded).accepted || decoded_book.live_order_count() != 1U) {
        std::cerr << "DecodedTick direct apply failed\n";
        return 1;
    }
    sse_tick::OrderBook short_security_book("60000");
    if (short_security_book.apply(decoded).accepted) {
        std::cerr << "DecodedTick security length was not checked\n";
        return 1;
    }
    sse_live::DecodedTick unterminated = decoded;
    unterminated.security_id[6] = '0';
    unterminated.security_id[7] = '0';
    unterminated.tick_index = 2;
    unterminated.provider_sequence = 2;
    unterminated.app_seq_num = 2;
    if (decoded_book.apply(unterminated).accepted) {
        std::cerr << "Unterminated DecodedTick security was accepted\n";
        return 1;
    }

    sse_tick::FlowStats window;
    window.events.reserve(32);
    const std::size_t initial_capacity = window.events.capacity();
    fast.take_flow_window(&window);
    if (window.events.empty()) {
        std::cerr << "flow window consumption failed\n";
        return 1;
    }
    fast.take_flow_window(&window);
    if (window.events.capacity() < initial_capacity) {
        std::cerr << "flow event buffer was not reused\n";
        return 1;
    }
    return 0;
}
