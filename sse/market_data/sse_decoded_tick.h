#ifndef SSE_T0_DECODED_TICK_H
#define SSE_T0_DECODED_TICK_H

#include <cstdint>

namespace sse_live {

// Fixed-layout decoded tick for zero-allocation handoff from the decoder.
// Keep this field order in sync with FastTick in the benchmark prototype.
struct DecodedTick {
    char security_id[8];
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

}  // namespace sse_live

#endif  // SSE_T0_DECODED_TICK_H
