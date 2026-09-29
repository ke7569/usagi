#include "sse/auction/sse_opening_boundary.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << "\n";
        std::exit(1);
    }
}

sse_live::TickEvent event(char type, std::uint32_t channel, std::uint64_t seq,
                          std::uint32_t price, std::uint64_t qty,
                          unsigned char side) {
    sse_live::TickEvent value;
    value.security_id = "600519";
    value.channel_no = channel;
    value.app_seq_num = seq;
    value.time_of_day_micros = (9ULL * 3600ULL + 25ULL * 60ULL) * 1000000ULL + 600000ULL;
    value.event_type = type;
    value.buy_order_no = type == 'T' ? 11U : 0U;
    value.sell_order_no = type == 'T' ? 12U : 0U;
    value.price_raw = price;
    value.quantity_raw = qty;
    value.amount_raw = type == 'T' ? price * qty / 1000U : 0U;
    value.side = static_cast<char>(side);
    return value;
}

}  // namespace

int main() {
    sse_live::OpeningBoundaryTracker tracker;
    tracker.observe(event('T', 1, 191322, 1291000, 100000, 2));
    tracker.observe(event('T', 1, 191323, 1291000, 200000, 2));
    tracker.observe(event('S', 1, 191324, 0, 0, 3));
    sse_live::OpeningBoundary boundary;
    require(tracker.get("600519", &boundary), "missing boundary");
    require(boundary.valid && boundary.quality == "ok", "valid boundary rejected");
    require(boundary.opening_auction_last_app_seq == 191323, "wrong inferred boundary");
    require(boundary.opening_trade_rows == 2 && boundary.opening_quantity_raw == 300000,
            "wrong opening aggregate");

    sse_live::OpeningBoundaryTracker no_trade;
    no_trade.observe(event('S', 2, 42, 0, 0, 3));
    require(no_trade.get("600519", &boundary), "missing no-trade boundary");
    require(boundary.valid && !boundary.has_opening_trade &&
            boundary.opening_auction_last_app_seq == 41 && boundary.quality == "no_opening_trade",
            "no-trade boundary mismatch");

    sse_live::OpeningBoundaryTracker bad;
    bad.observe(event('T', 3, 100, 10000, 100000, 2));
    bad.observe(event('S', 3, 102, 0, 0, 3));
    require(bad.get("600519", &boundary) && !boundary.valid &&
            boundary.quality == "status_not_adjacent_to_last_trade",
            "sequence gap accepted");
    std::cout << "sse_opening_boundary_test: ok\n";
    return 0;
}
