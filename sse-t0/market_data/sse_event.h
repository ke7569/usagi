#ifndef SSE_PIPELINE_EVENT_H
#define SSE_PIPELINE_EVENT_H

#include <cstdint>
#include <type_traits>

namespace sse_pipeline {

static const std::uint32_t kEventVersion = 1;
static const std::uint32_t kUnassignedShard = 0xffffffffU;
enum EventKind { kTick = 1, kSnapshot = 2, kTickSample = 3 };
enum EventFlags { kLateTick = 1 };

// Fixed-width, little-endian event envelope. Payload retains the complete
// wire record; receive timestamps and exchange ChannelNo survive every hop.
struct Event {
    std::uint64_t event_id;
    std::uint64_t receive_realtime_ns;
    std::uint64_t receive_mono_ns;
    std::uint64_t exchange_time_us;
    std::uint64_t sequence;
    std::uint32_t trading_day;
    std::uint32_t shard_id;
    std::uint16_t channel_no;
    std::uint16_t payload_size;
    std::uint8_t kind;
    std::uint8_t flags;
    std::uint16_t version;
    char security_id[8];
    unsigned char payload[440];
};

static_assert(sizeof(Event) == 504, "SSE event ABI must remain fixed");
static_assert(std::is_trivial<Event>::value && std::is_standard_layout<Event>::value,
              "SSE events must be copyable without allocation");

}  // namespace sse_pipeline
#endif
