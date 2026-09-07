#ifndef SNAP_LATENCY_H
#define SNAP_LATENCY_H

#include <cstdint>
#include <string>
#include <vector>

struct SnapLatencyStats {
    uint64_t process_order_count = 0;
    uint64_t process_order_ns = 0;
    uint64_t process_trade_count = 0;
    uint64_t process_trade_ns = 0;

    uint64_t insert_quote_count = 0;
    uint64_t insert_quote_ns = 0;
    uint64_t assign_snap_count = 0;
    uint64_t assign_snap_ns = 0;
    uint64_t assign_instrument_parse_ns = 0;
    uint64_t assign_time_parse_ns = 0;
    uint64_t assign_get_snap_ns = 0;

    uint64_t engine_insert_quote_count = 0;
    uint64_t engine_insert_quote_ns = 0;
    uint64_t engine_order_match_count = 0;
    uint64_t engine_order_match_ns = 0;
    uint64_t engine_get_snap_count = 0;
    uint64_t engine_get_snap_ns = 0;

    bool dumped = false;

    std::vector<std::string> format_lines() const;
    void mark_dumped();
    ~SnapLatencyStats();
};

uint64_t snap_now_ns();
bool snap_latency_enabled();
SnapLatencyStats& snap_latency_stats();

#endif // SNAP_LATENCY_H
