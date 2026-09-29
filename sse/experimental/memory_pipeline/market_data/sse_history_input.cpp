#include "sse_history_input.h"
#include "sse_event_codec.h"
#include "sse_primary_decoder.h"
#include <algorithm>
#include <fstream>
#include <queue>
#include <memory>

namespace sse_pipeline {
namespace {
struct Input {
    std::ifstream file;
    std::uint64_t record_bytes, records, cursor;
    sse_live::DiskRecordHeader header;
    unsigned char payload[440];
    explicit Input(const std::string& path) : file(path.c_str(), std::ios::binary), record_bytes(0), records(0), cursor(0), header() {}
    bool header_at(std::uint64_t index) {
        file.clear(); file.seekg(index * record_bytes);
        return static_cast<bool>(file.read(reinterpret_cast<char*>(&header), sizeof(header)));
    }
    bool open(std::uint64_t start) {
        if (!file.read(reinterpret_cast<char*>(&header), sizeof(header))) return false;
        if (header.magic != sse_live::kDiskRecordMagic || header.header_size != sizeof(header) ||
            (header.payload_length != 72 && header.payload_length != 440)) return false;
        record_bytes = sizeof(header) + header.payload_length;
        file.seekg(0, std::ios::end);
        const auto bytes = file.tellg();
        if (bytes < 0 || static_cast<std::uint64_t>(bytes) % record_bytes != 0) return false;
        records = static_cast<std::uint64_t>(bytes) / record_bytes;
        std::uint64_t first=0,last=records;
        while (first < last) {
            const auto mid = first + (last-first)/2;
            if (!header_at(mid)) return false;
            if (header.realtime_ns < start) first=mid+1; else last=mid;
        }
        cursor=first;
        file.clear(); file.seekg(cursor * record_bytes);
        return true;
    }
    bool next() {
        if (cursor == records) return false;
        ++cursor;
        if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
            header.magic != sse_live::kDiskRecordMagic || sizeof(header)+header.payload_length != record_bytes)
            return false;
        return static_cast<bool>(file.read(reinterpret_cast<char*>(payload), header.payload_length));
    }
};
}
bool load_history(const std::vector<std::string>& files, std::uint64_t start_ns,
                  std::uint64_t stop_ns, std::uint32_t day, HistoryInput* output, std::string* error) {
    output->events.clear(); output->rejected=0;
    if (files.empty() || stop_ns <= start_ns) { *error="invalid history interval/files"; return false; }
    std::vector<std::unique_ptr<Input> > inputs;
    typedef std::pair<std::uint64_t,std::size_t> Head;
    std::priority_queue<Head,std::vector<Head>,std::greater<Head> > heap;
    for (const auto& path : files) {
        inputs.emplace_back(new Input(path));
        if (!inputs.back()->open(start_ns)) { *error="invalid fixed-record input: "+path; return false; }
        if (inputs.back()->next()) heap.push(Head(inputs.back()->header.realtime_ns, inputs.size()-1));
    }
    while (!heap.empty()) {
        const Head head=heap.top(); heap.pop();
        if (head.first >= stop_ns) break;
        Input& input=*inputs[head.second];
        Event event;
        std::string reason;
        if (decode_event(input.payload,input.header.payload_length,input.header.realtime_ns,
                         input.header.monotonic_ns,day,&event,&reason)) output->events.push_back(event);
        else ++output->rejected;
        const std::uint64_t previous=input.header.realtime_ns;
        if (input.next()) {
            if (input.header.realtime_ns < previous) { *error="history file timestamps regress"; return false; }
            heap.push(Head(input.header.realtime_ns,head.second));
        }
    }
    if (output->events.empty()) { *error="history interval contains no valid Shanghai events"; return false; }
    output->start_realtime_ns=output->events.front().receive_realtime_ns;
    output->stop_realtime_ns=output->events.back().receive_realtime_ns;
    return true;
}
}
