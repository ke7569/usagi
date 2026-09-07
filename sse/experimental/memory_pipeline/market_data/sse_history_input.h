#ifndef SSE_HISTORY_INPUT_H
#define SSE_HISTORY_INPUT_H
#include "sse_event.h"
#include <string>
#include <vector>
namespace sse_pipeline {
struct HistoryInput {
    std::vector<Event> events;
    std::uint64_t rejected=0;
    std::uint64_t start_realtime_ns=0, stop_realtime_ns=0;
};
bool load_history(const std::vector<std::string>& files, std::uint64_t start_ns,
                  std::uint64_t stop_ns, std::uint32_t day, HistoryInput* output, std::string* error);
}
#endif
