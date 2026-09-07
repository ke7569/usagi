#include "sse/market_data/sse_primary_decoder.h"

#include <ctime>

namespace sse_live {
namespace {

std::uint32_t u32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t u64(const unsigned char* p) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
    return value;
}

std::string ascii(const unsigned char* p, std::size_t length) {
    std::string value;
    for (std::size_t i = 0; i < length && p[i] != 0; ++i) {
        if (p[i] >= '0' && p[i] <= '9') value.push_back(static_cast<char>(p[i]));
        else if (p[i] != ' ') return std::string();
    }
    return value;
}

bool hms_micros(std::uint32_t raw, bool centiseconds, std::uint64_t* result) {
    std::uint32_t subsecond = 0;
    if (centiseconds) {
        subsecond = raw % 100U;
        raw /= 100U;
    }
    const std::uint32_t second = raw % 100U;
    raw /= 100U;
    const std::uint32_t minute = raw % 100U;
    const std::uint32_t hour = raw / 100U;
    if (hour > 23U || minute > 59U || second > 59U) return false;
    *result = (static_cast<std::uint64_t>(hour) * 3600ULL +
               static_cast<std::uint64_t>(minute) * 60ULL + second) * 1000000ULL +
              static_cast<std::uint64_t>(subsecond) * 10000ULL;
    return true;
}

void fail(const char* message, std::string* error) {
    if (error) *error = message;
}

}  // namespace

bool is_sse_stock(const std::string& security_id) {
    return security_id.size() == 6U &&
           ((security_id[0] == '6' && security_id[1] == '0') ||
            (security_id[0] == '6' && security_id[1] == '8'));
}

bool decode_primary_tick(const unsigned char* payload, std::size_t length,
                         TickEvent* event, std::string* error, bool equities_only) {
    if (error) error->clear();
    if (!payload || !event || length != 72U || payload[8] != 0x3eU) {
        fail("primary tick ABI mismatch", error);
        return false;
    }
    const std::string security = ascii(payload + 21, 8U);
    std::uint64_t time_of_day = 0;
    if (security.size() != 6U || (equities_only && !is_sse_stock(security)) ||
        !hms_micros(u32(payload + 30), true, &time_of_day)) {
        fail("invalid primary tick security/time", error);
        return false;
    }
    const char event_type = static_cast<char>(payload[34]);
    if (event_type != 'A' && event_type != 'D' && event_type != 'T' && event_type != 'S') {
        fail("unknown primary tick event type", error);
        return false;
    }
    event->security_id = security;
    // EFH sse_hpf_tick_merge: m_sequence@0, m_tick_index@9,
    // m_channel_num@17 (uint16). Offset +4 is reserved.
    event->provider_sequence = u32(payload + 0);
    event->tick_index = u64(payload + 9);
    event->app_seq_num = event->tick_index;  // legacy internal name
    event->channel_no = static_cast<std::uint32_t>(payload[17]) |
                        (static_cast<std::uint32_t>(payload[18]) << 8);
    event->time_of_day_micros = time_of_day;
    event->event_type = event_type;
    event->buy_order_no = u64(payload + 35);
    event->sell_order_no = u64(payload + 43);
    event->price_raw = u32(payload + 51);
    event->quantity_raw = u64(payload + 55);
    event->amount_raw = u64(payload + 63);
    event->side = static_cast<char>(payload[71]);
    return true;
}

bool decode_primary_snapshot(const unsigned char* payload, std::size_t length,
                             Snapshot* snapshot, std::string* error, bool equities_only) {
    if (error) error->clear();
    if (!payload || !snapshot || length != 440U || payload[8] != 0x27U) {
        fail("primary snapshot ABI mismatch", error);
        return false;
    }
    const std::string security = ascii(payload + 30, 8U);
    std::uint64_t time_of_day = 0;
    // m_send_time@16 is transport time; m_quote_update_time@26 is the
    // market-data update time.
    const std::uint32_t time_raw = u32(payload + 26);
    if (security.size() != 6U || (equities_only && !is_sse_stock(security)) ||
        !hms_micros(time_raw, false, &time_of_day)) {
        fail("invalid primary snapshot security/time", error);
        return false;
    }
    snapshot->security_id = security;
    snapshot->channel_no = 0;  // sse_hpf_lev2 contains no m_channel_num
    snapshot->provider_sequence = u32(payload + 0);
    snapshot->msg_seq_id = u32(payload + 21);
    snapshot->sequence = snapshot->msg_seq_id;
    snapshot->exchange_time_raw = time_raw;
    snapshot->time_of_day_micros = time_of_day;
    snapshot->pre_close_price = static_cast<double>(u32(payload + 42)) / 1000.0;
    snapshot->open_price = static_cast<double>(u32(payload + 46)) / 1000.0;
    snapshot->high_price = static_cast<double>(u32(payload + 50)) / 1000.0;
    snapshot->low_price = static_cast<double>(u32(payload + 54)) / 1000.0;
    snapshot->last_price = static_cast<double>(u32(payload + 58)) / 1000.0;

    // Compact primary adapter ABI, verified against the 2026-08-17/18 feed:
    // offset 66 NumTrades(u32), 70 TradeVolume(u32), 74 volume*1000(u64),
    // 82 turnover*100000(u64), followed by aggregate quote fields.
    snapshot->volume = static_cast<std::int64_t>(u64(payload + 74) / 1000ULL);
    snapshot->turnover = static_cast<double>(u64(payload + 82)) / 100000.0;
    for (std::size_t level = 0; level < 5U; ++level) {
        const std::size_t bid = 124U + level * 16U;
        const std::size_t ask = 124U + (10U + level) * 16U;
        snapshot->bid_prices[level] = static_cast<double>(u32(payload + bid)) / 1000.0;
        snapshot->bid_volumes[level] = static_cast<std::int64_t>(u64(payload + bid + 4U) / 1000ULL);
        snapshot->ask_prices[level] = static_cast<double>(u32(payload + ask)) / 1000.0;
        snapshot->ask_volumes[level] = static_cast<std::int64_t>(u64(payload + ask + 4U) / 1000ULL);
    }
    return true;
}

std::uint32_t local_trading_date(std::uint64_t realtime_ns) {
    const std::time_t seconds = static_cast<std::time_t>(realtime_ns / 1000000000ULL);
    std::tm value;
    if (localtime_r(&seconds, &value) == 0) return 0;
    return static_cast<std::uint32_t>((value.tm_year + 1900) * 10000 +
                                      (value.tm_mon + 1) * 100 + value.tm_mday);
}

}  // namespace sse_live
