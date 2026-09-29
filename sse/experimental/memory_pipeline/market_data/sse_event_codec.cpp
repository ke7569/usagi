#include "sse_event_codec.h"
#include "sse_primary_decoder.h"
#include <cstring>

namespace sse_pipeline {
bool decode_event(const unsigned char* record, std::size_t size,
                  std::uint64_t realtime_ns, std::uint64_t monotonic_ns,
                  std::uint32_t day, Event* event, std::string* error) {
    if (!event || !record || (size != 72 && size != 440)) {
        if (error) *error = "invalid SSE record size";
        return false;
    }
    *event = Event();
    event->version = kEventVersion;
    event->shard_id = kUnassignedShard;
    event->receive_realtime_ns = realtime_ns;
    event->receive_mono_ns = monotonic_ns;
    event->trading_day = day;
    event->payload_size = static_cast<std::uint16_t>(size);
    std::string symbol;
    if (size == 72) {
        sse_live::TickEvent tick;
        if (!sse_live::decode_primary_tick(record, size, &tick, error)) return false;
        if (tick.channel_no < 1 || tick.channel_no > 6 || tick.tick_index == 0) {
            if (error) *error = "SSE tick requires exchange ChannelNo 1..6 and a positive sequence";
            return false;
        }
        event->kind = kTick;
        event->channel_no = static_cast<std::uint16_t>(tick.channel_no);
        event->sequence = tick.tick_index;
        event->exchange_time_us = tick.time_of_day_micros;
        symbol = tick.security_id;
    } else {
        sse_live::Snapshot snapshot;
        if (!sse_live::decode_primary_snapshot(record, size, &snapshot, error)) return false;
        event->kind = kSnapshot;
        event->sequence = snapshot.sequence;
        event->exchange_time_us = snapshot.time_of_day_micros;
        symbol = snapshot.security_id;
    }
    std::memcpy(event->security_id, symbol.data(), symbol.size());
    std::memcpy(event->payload, record, size);
    return true;
}
}
