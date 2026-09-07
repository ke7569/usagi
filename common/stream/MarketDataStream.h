#ifndef T0_MARKET_DATA_STREAM_H
#define T0_MARKET_DATA_STREAM_H

#include "common/stream/UdpChannelRuntime.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace deepwin_market_data {

enum StreamEventKind { kDatagramEvent = 1, kIdleEvent = 2 };
enum StreamTimestampFlags { kKernelRealtimeTimestamp = 1, kUserspaceRealtimeTimestamp = 2 };

// Borrowed bytes are valid only during the callback. Channel IDs index the
// supplied channel vector. receive_batch is a syscall boundary, not a sample.
struct StreamEvent {
    StreamEventKind kind;
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::uint64_t realtime_ns;
    std::uint64_t receive_batch;
    std::uint32_t channel_id;
    std::uint32_t batch_index;
    std::uint32_t batch_size;
    std::uint32_t source_ipv4;
    std::uint16_t source_port;
    std::uint16_t timestamp_flags;
    const unsigned char* data;
    std::size_t size;
};

typedef std::function<void(const StreamEvent&)> StreamCallback;

struct StreamOptions {
    StreamOptions();
    std::size_t queue_capacity;
    std::size_t max_datagram_bytes;
    std::size_t receive_batch_size;
    int receive_buffer_bytes;
    std::uint64_t idle_gap_ns;
    std::uint64_t segment_bytes;
    int flush_interval_ms;
    int receive_cpu;
    int dispatch_cpu;
    int writer_cpu;
    std::string recording_directory;
    bool recording_required;
};

struct StreamHealth {
    bool input_valid;
    bool processing_valid;
    bool recording_failed;
    bool recording_required;
    // Necessary transport prerequisite only, not an account/strategy gate.
    bool permits_new_risk() const {
        return input_valid && processing_valid && !(recording_required && recording_failed);
    }
};

struct StreamStats {
    StreamStats();
    std::uint64_t received_datagrams;
    std::uint64_t receive_batches;
    std::uint64_t dispatched_events;
    std::uint64_t written_sequence;
    std::uint64_t durable_sequence;
    std::uint64_t ingress_high_water;
    std::uint64_t recording_high_water;
    std::uint64_t ingress_overflows;
    std::uint64_t recording_overflows;
    std::uint64_t kernel_drops;
    bool clean_recording;
};

class MarketDataStream {
public:
    MarketDataStream();
    ~MarketDataStream();
    MarketDataStream(const MarketDataStream&) = delete;
    MarketDataStream& operator=(const MarketDataStream&) = delete;

    // One receive producer, one serialized application callback, one optional
    // disk writer. stop() may be called by the callback or an external thread.
    bool run(const std::vector<ChannelSpec>& channels, const StreamOptions& options,
             const StreamCallback& callback, long duration_ms, std::string* error);
    void stop();
    bool stopping() const;
    bool ready() const;
    // Read after run/replay returns; these statistics are not a live view.
    // durable_sequence is a writer fdatasync watermark; replay leaves it zero.
    const StreamStats& stats() const;
    const std::vector<ChannelSpec>& channels() const;
    StreamHealth health() const;
    std::string recording_error() const;

    // Replays recorded idle events as well as datagrams, with no wall-clock
    // sleeps or implicit EOF flush. Corrupt/incomplete recordings are rejected.
    // A nonnegative required_idle_gap_ns is checked in preflight, before callbacks.
    bool replay(const std::string& directory, const StreamCallback& callback,
                std::string* error, std::int64_t required_idle_gap_ns = -1);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace deepwin_market_data
#endif
