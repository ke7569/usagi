#include "common/market_data/legacy/snap_latency.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace {
uint64_t avg_ns(uint64_t total, uint64_t count) {
    return count == 0 ? 0 : (total / count);
}
}

uint64_t snap_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool snap_latency_enabled() {
    static const bool enabled = []() {
        const char* env = std::getenv("T0_SNAP_LATENCY");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();
    return enabled;
}

SnapLatencyStats& snap_latency_stats() {
    static SnapLatencyStats stats;
    return stats;
}

std::vector<std::string> SnapLatencyStats::format_lines() const {
    std::vector<std::string> lines;
    if (!snap_latency_enabled()) {
        return lines;
    }
    if (process_order_count == 0 && process_trade_count == 0) {
        return lines;
    }

    std::ostringstream oss;
    oss << "[Timing][SnapGenerator][event]"
        << " process_order_avg_ns=" << avg_ns(process_order_ns, process_order_count)
        << " process_order_count=" << process_order_count
        << " process_trade_avg_ns=" << avg_ns(process_trade_ns, process_trade_count)
        << " process_trade_count=" << process_trade_count;
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][SnapGenerator][snap]"
        << " insert_quote_avg_ns=" << avg_ns(insert_quote_ns, insert_quote_count)
        << " insert_quote_count=" << insert_quote_count
        << " assign_snap_avg_ns=" << avg_ns(assign_snap_ns, assign_snap_count)
        << " assign_snap_count=" << assign_snap_count
        << " assign_instrument_parse_avg_ns=" << avg_ns(assign_instrument_parse_ns, assign_snap_count)
        << " assign_time_parse_avg_ns=" << avg_ns(assign_time_parse_ns, assign_snap_count)
        << " assign_get_snap_avg_ns=" << avg_ns(assign_get_snap_ns, assign_snap_count);
    lines.push_back(oss.str());
    oss.str("");
    oss.clear();

    oss << "[Timing][SnapGenerator][engine]"
        << " engine_insert_quote_avg_ns=" << avg_ns(engine_insert_quote_ns, engine_insert_quote_count)
        << " engine_insert_quote_count=" << engine_insert_quote_count
        << " engine_order_match_avg_ns=" << avg_ns(engine_order_match_ns, engine_order_match_count)
        << " engine_order_match_count=" << engine_order_match_count
        << " engine_get_snap_avg_ns=" << avg_ns(engine_get_snap_ns, engine_get_snap_count)
        << " engine_get_snap_count=" << engine_get_snap_count;
    lines.push_back(oss.str());
    return lines;
}

void SnapLatencyStats::mark_dumped() {
    dumped = true;
}

SnapLatencyStats::~SnapLatencyStats() {
    if (dumped) {
        return;
    }
    const auto lines = format_lines();
    for (const auto& line : lines) {
        std::cout << line << std::endl;
    }
}
