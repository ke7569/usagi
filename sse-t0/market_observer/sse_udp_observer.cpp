#include "UdpChannelRuntime.h"

#include <fstream>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <sched.h>

namespace {

std::string hex_prefix(const unsigned char* data, std::size_t size) {
    std::ostringstream out;
    const std::size_t limit = size < 64U ? size : 64U;
    for (std::size_t i = 0; i < limit; ++i) {
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(data[i]);
    }
    return out.str();
}

bool bind_process_to_cpu(int cpu, std::string* error) {
    if (cpu < 0) return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
        if (error) *error = std::string("sched_setaffinity failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: sse_udp_observer output.jsonl name group port [name group port ...] [--interface-ip IP] [--duration-ms N] [--cpu N]\n";
        return 2;
    }
    long duration_ms = 0;
    int cpu = -1;
    std::string interface_ip = "0.0.0.0";
    int channel_argc = 2;
    while (channel_argc < argc && std::string(argv[channel_argc]).find("--") != 0U) {
        ++channel_argc;
    }
    for (int i = channel_argc; i < argc; i += 2) {
        if (i + 1 >= argc) {
            std::cerr << "missing value for observer option\n";
            return 2;
        }
        const std::string option = argv[i];
        if (option == "--duration-ms") duration_ms = std::atol(argv[i + 1]);
        else if (option == "--interface-ip") interface_ip = argv[i + 1];
        else if (option == "--cpu") cpu = std::atoi(argv[i + 1]);
        else {
            std::cerr << "unknown observer option: " << option << "\n";
            return 2;
        }
    }
    if (channel_argc < 5 || ((channel_argc - 2) % 3) != 0) {
        std::cerr << "usage: sse_udp_observer output.jsonl name group port [name group port ...] [--interface-ip IP] [--duration-ms N] [--cpu N]\n";
        return 2;
    }

    std::ofstream file(argv[1], std::ios::out | std::ios::app);
    if (!file) return 3;
    std::vector<deepwin_market_data::ChannelSpec> channels;
    for (int i = 2; i < channel_argc; i += 3) {
        deepwin_market_data::ChannelSpec channel;
        channel.name = argv[i];
        channel.group = argv[i + 1];
        channel.port = std::atoi(argv[i + 2]);
        channel.interface_ip = interface_ip;
        channels.push_back(channel);
    }

    std::string error;
    if (!bind_process_to_cpu(cpu, &error)) {
        std::cerr << "sse_udp_observer: " << error << "\n";
        return 4;
    }
    deepwin_market_data::UdpChannelRuntime runtime;
    std::mutex output_mutex;
    std::uint64_t buffered_datagrams = 0U;
    const deepwin_market_data::DatagramCallback callback =
        [&file, &output_mutex, &buffered_datagrams](const deepwin_market_data::Datagram& datagram) {
            std::lock_guard<std::mutex> lock(output_mutex);
            file << "{\"ts_ns\":" << datagram.receive_ns
                 << ",\"monotonic_ns\":" << datagram.monotonic_ns
                 << ",\"channel\":\"" << datagram.channel
                 << "\",\"source_ip\":\"" << datagram.source_ip
                 << "\",\"source_port\":" << datagram.source_port
                 << ",\"length\":" << datagram.size
                 << ",\"prefix_hex\":\"" << hex_prefix(datagram.data, datagram.size)
                 << "\"}\n";
            // Per-datagram flushes add a write syscall to the receive path and
            // can starve the independent binary parser during a UDP burst.
            // A bounded batch retains crash visibility while keeping capture
            // throughput predictable.
            if (++buffered_datagrams % 1024U == 0U) file.flush();
        };
    std::cerr << "sse_udp_observer running cpu=" << cpu
              << " flush_batch=1024; press Ctrl-C to stop\n";
    if (!runtime.run(channels, callback, duration_ms, &error)) {
        std::cerr << "sse_udp_observer: " << error << "\n";
        return 4;
    }
    file.flush();
    return 0;
}
