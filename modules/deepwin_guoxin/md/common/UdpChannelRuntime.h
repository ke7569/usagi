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

    Datagram() : source_port(0U), receive_ns(0), monotonic_ns(0), data(0), size(0U) {}
};

typedef std::function<void(const Datagram&)> DatagramCallback;

class UdpChannelRuntime {
public:
    UdpChannelRuntime();
    ~UdpChannelRuntime();

    UdpChannelRuntime(const UdpChannelRuntime&) = delete;
    UdpChannelRuntime& operator=(const UdpChannelRuntime&) = delete;

    // Runs one receive worker per channel. A positive duration is useful for
    // offline probes; zero means run until the process is terminated.
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
