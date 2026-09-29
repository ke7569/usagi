#include "common/stream/MarketDataStream.h"
#include "sse/runtime/sse_cpu_affinity.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

std::string usage() {
    return "usage: sse_udp_observer output.jsonl name group port [name group port ...] "
           "[--interface-ip IP] [--duration-ms N] [--cpu-list CPU,CPU | --cpus CPU,CPU]";
}

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool parse_duration(const std::string& text, long* duration, std::string* error) {
    if (text.empty()) return fail(error, "duration must be a non-negative integer");
    char* end = 0;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' || value < 0)
        return fail(error, "duration must be a non-negative integer");
    *duration = value;
    return true;
}

bool parse_cpu(const std::string& text, int* cpu, std::string* error) {
    if (text == "-1") {
        *cpu = -1;
        return true;
    }
    if (text.empty() || text[0] < '0' || text[0] > '9')
        return fail(error, "CPU must be a non-negative integer or -1");
    for (std::size_t i = 1U; i < text.size(); ++i)
        if (text[i] < '0' || text[i] > '9')
            return fail(error, "CPU must be a non-negative integer or -1");
    char* end = 0;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
        value < 0 || value >= CPU_SETSIZE)
        return fail(error, "CPU is outside the available CPU range");
    *cpu = static_cast<int>(value);
    return true;
}

bool parse_cpu_pair(const std::string& text, std::vector<int>* cpus,
                    std::string* error) {
    cpus->clear();
    const std::size_t comma = text.find(',');
    if (comma == std::string::npos || text.find(',', comma + 1U) != std::string::npos)
        return fail(error, "expected exactly two CPUs in CPU,CPU (use -1 for auto)");
    int receive = -1;
    int dispatch = -1;
    if (!parse_cpu(text.substr(0U, comma), &receive, error) ||
        !parse_cpu(text.substr(comma + 1U), &dispatch, error))
        return false;
    cpus->push_back(receive);
    cpus->push_back(dispatch);
    return true;
}

std::string source_ip(std::uint32_t address) {
    in_addr value;
    value.s_addr = address;
    char text[INET_ADDRSTRLEN] = {};
    return ::inet_ntop(AF_INET, &value, text, sizeof(text)) ? text : std::string();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << usage() << '\n';
        return 2;
    }
    long duration_ms = 0;
    std::string interface_ip = "0.0.0.0";
    std::string cpu_text;
    bool cpu_option = false;
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
        if (option == "--duration-ms") {
            std::string error;
            if (!parse_duration(argv[i + 1], &duration_ms, &error)) {
                std::cerr << "sse_udp_observer: " << error << '\n';
                return 2;
            }
        }
        else if (option == "--interface-ip") interface_ip = argv[i + 1];
        else if (option == "--cpu-list" || option == "--cpus") {
            if (cpu_option) {
                std::cerr << "duplicate CPU option\n";
                return 2;
            }
            cpu_option = true;
            cpu_text = argv[i + 1];
        }
        else {
            std::cerr << "unknown observer option: " << option << "\n";
            return 2;
        }
    }
    if (channel_argc < 5 || ((channel_argc - 2) % 3) != 0) {
        std::cerr << usage() << '\n';
        return 2;
    }

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
    std::vector<int> requested(2U, -1);
    if (cpu_option && !parse_cpu_pair(cpu_text, &requested, &error)) {
        std::cerr << "sse_udp_observer: " << error << '\n';
        return 2;
    }
    sse_cpu::Lease lease;
    if (!lease.acquire(requested, &error) || lease.cpus().size() != 2U) {
        if (error.empty()) error = "CPU affinity lease did not return receive and dispatch CPUs";
        std::cerr << "sse_udp_observer: " << error << "\n";
        return 4;
    }
    const sse_cpu::Cpu& receive_cpu = lease.cpus()[0];
    const sse_cpu::Cpu& dispatch_cpu = lease.cpus()[1];
    std::cerr << "sse_udp_observer affinity receive_cpu=" << receive_cpu.id
              << " receive_l3=" << receive_cpu.l3
              << " dispatch_cpu=" << dispatch_cpu.id
              << " dispatch_l3=" << dispatch_cpu.l3 << '\n';

    std::ofstream file(argv[1], std::ios::out | std::ios::app);
    if (!file) return 3;
    deepwin_market_data::StreamOptions options;
    options.queue_capacity = 1024U;
    options.max_datagram_bytes = 65536U;
    options.idle_gap_ns = 0U;
    options.receive_cpu = receive_cpu.id;
    options.dispatch_cpu = dispatch_cpu.id;
    options.writer_cpu = -1;
    options.recording_directory.clear();
    options.recording_required = false;
    std::uint64_t buffered_datagrams = 0U;
    const deepwin_market_data::StreamCallback callback =
        [&file, &channels, &buffered_datagrams](const deepwin_market_data::StreamEvent& event) {
            if (event.kind != deepwin_market_data::kDatagramEvent) return;
            if (event.channel_id >= channels.size() || (event.size != 0U && event.data == 0))
                throw std::runtime_error("observer received invalid datagram metadata");
            file << "{\"ts_ns\":" << event.realtime_ns
                 << ",\"monotonic_ns\":" << event.monotonic_ns
                 << ",\"channel\":\"" << channels[event.channel_id].name
                 << "\",\"source_ip\":\"" << source_ip(event.source_ipv4)
                 << "\",\"source_port\":" << event.source_port
                 << ",\"length\":" << event.size
                 << ",\"prefix_hex\":\"" << hex_prefix(event.data, event.size)
                 << "\"}\n";
            if (++buffered_datagrams % 1024U == 0U) file.flush();
            if (!file) throw std::runtime_error("observer output write failed");
        };
    deepwin_market_data::MarketDataStream runtime;
    std::cerr << "sse_udp_observer running serialized_datagrams=1 flush_batch=1024; press Ctrl-C to stop\n";
    if (!runtime.run(channels, options, callback, duration_ms, &error)) {
        std::cerr << "sse_udp_observer: " << error << "\n";
        return 4;
    }
    file.flush();
    if (!file) {
        std::cerr << "sse_udp_observer: final output flush failed\n";
        return 4;
    }
    return 0;
}
