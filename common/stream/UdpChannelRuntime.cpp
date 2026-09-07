#include "common/stream/UdpChannelRuntime.h"
#include "common/stream/MarketDataStream.h"

#include <arpa/inet.h>

namespace deepwin_market_data {

struct UdpChannelRuntime::Impl {
    MarketDataStream stream;
};

UdpChannelRuntime::UdpChannelRuntime() : impl_(new Impl()) {}
UdpChannelRuntime::~UdpChannelRuntime() { delete impl_; }
void UdpChannelRuntime::stop() { impl_->stream.stop(); }

bool UdpChannelRuntime::run(const std::vector<ChannelSpec>& channels,
                            const DatagramCallback& callback,
                            long duration_ms, std::string* error) {
    if (!callback) {
        if (error) *error = "UDP channel callback is required";
        return false;
    }
    StreamOptions options;
    options.recording_required = false;
    options.max_datagram_bytes = 65536;
    options.queue_capacity = 1024;
    options.idle_gap_ns = 0;
    Datagram datagram;
    std::uint32_t last_address = 0;
    const StreamCallback adapter = [&](const StreamEvent& event) {
        if (event.kind != kDatagramEvent) return;
        datagram.channel = channels[event.channel_id].name;
        if (event.source_ipv4 != last_address || datagram.source_ip.empty()) {
            in_addr address;
            address.s_addr = event.source_ipv4;
            char text[INET_ADDRSTRLEN];
            if (::inet_ntop(AF_INET, &address, text, sizeof(text))) datagram.source_ip = text;
            last_address = event.source_ipv4;
        }
        datagram.source_port = event.source_port;
        datagram.receive_ns = event.realtime_ns;
        datagram.monotonic_ns = event.monotonic_ns;
        datagram.sequence = event.sequence;
        datagram.receive_batch = event.receive_batch;
        datagram.channel_id = event.channel_id;
        datagram.batch_index = event.batch_index;
        datagram.batch_size = event.batch_size;
        datagram.timestamp_flags = event.timestamp_flags;
        datagram.data = event.data;
        datagram.size = event.size;
        callback(datagram);
    };
    return impl_->stream.run(channels, options, adapter, duration_ms, error);
}

}  // namespace deepwin_market_data
