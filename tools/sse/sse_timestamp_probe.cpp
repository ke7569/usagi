#include "common/stream/MarketDataStream.h"
#include "sse/runtime/sse_cpu_affinity.h"
#include "third_party/nlohmann/json.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) try {
    if (argc != 6) throw std::runtime_error("usage: sse_timestamp_probe INTERFACE INTERFACE_IP GROUP PORT DURATION_MS");
    deepwin_market_data::ChannelSpec channel;
    channel.name = "hardware_probe"; channel.interface_ip = argv[2]; channel.group = argv[3];
    channel.port = std::stoi(argv[4]);
    const long duration = std::stol(argv[5]);
    if (duration < 1 || duration > 60000) throw std::runtime_error("probe duration must be 1..60000 ms");
    deepwin_market_data::StreamOptions options;
    options.hardware_timestamp_interface = argv[1];
    options.recording_required = false; options.idle_gap_ns = 0;
    sse_cpu::Lease cpus; std::string error;
    if (!cpus.acquire({-1, -1}, &error)) throw std::runtime_error(error);
    options.receive_cpu = cpus.cpus()[0].id; options.dispatch_cpu = cpus.cpus()[1].id;
    deepwin_market_data::MarketDataStream stream;
    std::uint64_t datagrams = 0, hardware = 0, kernel = 0;
    nlohmann::json samples = nlohmann::json::array();
    const bool ok = stream.run({channel}, options, [&](const deepwin_market_data::StreamEvent& event) {
        if (event.kind != deepwin_market_data::kDatagramEvent) return;
        ++datagrams;
        hardware += (event.timestamp_flags & deepwin_market_data::kHardwareReceiveTimestamp) != 0;
        kernel += (event.timestamp_flags & deepwin_market_data::kKernelRealtimeTimestamp) != 0;
        if (samples.size() < 16) samples.push_back({
            {"sequence", event.sequence}, {"payload_bytes", event.size}, {"timestamp_flags", event.timestamp_flags},
            {"hardware_ns", std::to_string(event.hardware_ns)},
            {"kernel_realtime_ns", std::to_string(event.realtime_ns)},
            {"application_realtime_ns", std::to_string(event.application_realtime_ns)},
            {"application_monotonic_ns", std::to_string(event.monotonic_ns)},
            {"phc_index", event.hardware_clock_index}});
    }, duration, &error);
    nlohmann::json output = {{"ok", ok}, {"error", error}, {"datagrams", datagrams},
        {"hardware_timestamps", hardware}, {"kernel_timestamps", kernel},
        {"missing_hardware", datagrams - hardware}, {"kernel_drops", stream.stats().kernel_drops},
        {"ingress_overflows", stream.stats().ingress_overflows}, {"samples", samples}};
    std::cout << output.dump(2) << '\n';
    return ok && hardware && kernel == datagrams ? 0 : 1;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
