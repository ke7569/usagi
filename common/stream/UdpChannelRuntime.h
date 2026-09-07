#ifndef DEEPWIN_MARKET_DATA_UDP_CHANNEL_RUNTIME_H
#define DEEPWIN_MARKET_DATA_UDP_CHANNEL_RUNTIME_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace deepwin_market_data {

// Exchange-neutral socket settings. The decoder must remain outside this
// class because SSE and SZE wire records are different protocols.
struct ChannelSpec {
    std::string name;
    std::string group;
    int port;
    std::string interface_ip;

    ChannelSpec() : port(0), interface_ip("0.0.0.0") {}
};

struct Datagram {
    std::string channel;
    std::string source_ip;
    std::uint16_t source_port;
    std::int64_t receive_ns;
    std::int64_t monotonic_ns;
    const unsigned char* data;
    std::size_t size;
    std::uint64_t sequence;
    std::uint64_t receive_batch;
    std::uint32_t channel_id;
    std::uint32_t batch_index;
    std::uint32_t batch_size;
    std::uint16_t timestamp_flags;

    Datagram() : source_port(0U), receive_ns(0), monotonic_ns(0), data(0), size(0U),
        sequence(0), receive_batch(0), channel_id(0), batch_index(0), batch_size(0), timestamp_flags(0) {}
};

typedef std::function<void(const Datagram&)> DatagramCallback;

class UdpChannelRuntime {
public:
    UdpChannelRuntime();
    ~UdpChannelRuntime();

    UdpChannelRuntime(const UdpChannelRuntime&) = delete;
    UdpChannelRuntime& operator=(const UdpChannelRuntime&) = delete;

    // Compatibility wrapper over MarketDataStream's serialized callback.
    // New consumers use StreamEvent directly to retain recorded idle events.
    bool run(const std::vector<ChannelSpec>& channels,
             const DatagramCallback& callback,
             long duration_ms,
             std::string* error);
    void stop();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace deepwin_market_data

#endif  // DEEPWIN_MARKET_DATA_UDP_CHANNEL_RUNTIME_H
