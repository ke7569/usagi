#ifndef SSE_EVENT_CODEC_H
#define SSE_EVENT_CODEC_H
#include "sse_event.h"
#include <cstddef>
#include <string>

namespace sse_pipeline {
bool decode_event(const unsigned char* record, std::size_t size,
                  std::uint64_t realtime_ns, std::uint64_t monotonic_ns,
                  std::uint32_t day, Event* event, std::string* error);
}
#endif
