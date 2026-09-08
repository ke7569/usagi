#ifndef SSE_T0_PRIMARY_DECODER_H
#define SSE_T0_PRIMARY_DECODER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include "sse/market_data/sse_decoded_tick.h"

namespace sse_live {

static const std::uint32_t kDiskRecordMagic = 0x31524353U;

#pragma pack(push, 1)
struct DiskRecordHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t header_size;
    std::uint64_t realtime_ns;
    std::uint64_t monotonic_ns;
    std::uint64_t arrival_index;
    std::uint64_t datagram_index;
    std::uint32_t capture_sequence;
    std::uint32_t channel_no;
    std::uint32_t source_ipv4;
    std::uint16_t source_port;
    std::uint16_t record_index;
    std::uint16_t record_count;
    std::uint16_t payload_length;
    std::uint8_t message_type;
    std::uint8_t flags;
    std::uint16_t reserved;
};
#pragma pack(pop)

struct TickEvent {
    std::string security_id;
    std::uint32_t channel_no;
    std::uint32_t provider_sequence;
    std::uint64_t tick_index;
    std::uint64_t app_seq_num;
    std::uint64_t time_of_day_micros;
    char event_type;
    std::uint64_t buy_order_no;
    std::uint64_t sell_order_no;
    std::uint32_t price_raw;
    std::uint64_t quantity_raw;
    std::uint64_t amount_raw;
    char side;
};

struct Snapshot {
    std::string security_id;
    // sse_hpf_lev2 has no exchange ChannelNo.
    std::uint32_t channel_no;
    std::uint32_t provider_sequence;
    std::uint32_t msg_seq_id;
    std::uint64_t sequence;
    std::uint32_t exchange_time_raw;
    std::uint64_t time_of_day_micros;
    double pre_close_price;
    double open_price;
    double high_price;
    double low_price;
    double last_price;
    std::int64_t volume;
    double turnover;
    double bid_prices[5];
    std::int64_t bid_volumes[5];
    double ask_prices[5];
    std::int64_t ask_volumes[5];
};

bool is_sse_stock(const std::string& security_id);
// Full-channel consumers decode non-target securities to preserve channel
// continuity, then filter their configured universe before book mutation.
bool decode_primary_tick(const unsigned char* payload, std::size_t length,
                         TickEvent* event, std::string* error = 0,
                         bool equities_only = true);
enum TickDecodeError {
    kTickDecodeOk = 0,
    kTickDecodeAbi = 1,
    kTickDecodeSecurityOrTime = 2,
    kTickDecodeEventType = 3,
    kTickDecodeCapacity = 4
};
// Allocation-free Shanghai hot path. The legacy owned-string interface above
// remains available for old tools. Failed output must not be consumed.
bool decode_primary_tick_fast(const unsigned char* payload, std::size_t length,
                             DecodedTick* event, TickDecodeError* error = 0,
                             bool equities_only = true);
// count remains zero on failure, even when earlier output slots were written.
// This validates the complete datagram before callers publish any records.
bool decode_primary_tick_packet(const unsigned char* payload, std::size_t length,
                               DecodedTick* events, std::size_t capacity,
                               std::size_t* count, TickDecodeError* error = 0,
                               std::size_t* bad_offset = 0,
                               bool equities_only = false);
const char* tick_decode_error_text(TickDecodeError error);
void materialize_tick(const DecodedTick& input, TickEvent* output);
// Recognizes the provisioned feed's duplicated 16-byte heartbeat frames.
// It carries no exchange ChannelNo and must not advance channel tick sequence.
bool is_primary_heartbeat(const unsigned char* payload, std::size_t length);
bool decode_primary_snapshot(const unsigned char* payload, std::size_t length,
                             Snapshot* snapshot, std::string* error = 0,
                             bool equities_only = true);
std::uint32_t local_trading_date(std::uint64_t realtime_ns);

}  // namespace sse_live

#endif  // SSE_T0_PRIMARY_DECODER_H
