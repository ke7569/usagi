#include "common/stream/MarketDataStream.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {

using deepwin_market_data::ChannelSpec;
using deepwin_market_data::MarketDataStream;
using deepwin_market_data::StreamEvent;
using deepwin_market_data::StreamOptions;

ChannelSpec loopback_channel() {
    const int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(socket >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    ::close(socket);
    ChannelSpec result;
    result.name = "wait-test";
    result.group = "127.0.0.1";
    result.interface_ip = "127.0.0.1";
    result.port = ntohs(address.sin_port);
    return result;
}

void send_packet(int socket, const ChannelSpec& channel, unsigned char value) {
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(channel.port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::sendto(socket, &value, sizeof(value), 0,
                    reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 1);
}

bool wait_for(const std::atomic<bool>& value, unsigned milliseconds) {
    for (unsigned i = 0U; i < milliseconds; ++i) {
        if (value.load(std::memory_order_acquire)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load(std::memory_order_acquire);
}

bool wait_for_count(const std::atomic<unsigned>& value, unsigned target,
                    unsigned milliseconds) {
    for (unsigned i = 0U; i < milliseconds; ++i) {
        if (value.load(std::memory_order_acquire) >= target) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load(std::memory_order_acquire) >= target;
}

}  // namespace

int main() {
    const ChannelSpec channel = loopback_channel();
    StreamOptions options;
    options.queue_capacity = 64;
    options.max_datagram_bytes = 64;
    options.receive_batch_size = 4;
    options.receive_buffer_bytes = 1 << 20;
    options.idle_gap_ns = 5000U;
    options.recording_required = false;

    MarketDataStream stream;
    std::atomic<unsigned> polls(0U);
    std::atomic<unsigned> idles(0U);
    std::mutex mutex;
    std::vector<StreamEvent> events;
    std::vector<std::uint64_t> datagram_times;
    std::vector<std::uint64_t> idle_times;
    bool ok = false;
    std::string error;
    std::thread runner([&]() {
        ok = stream.run(std::vector<ChannelSpec>(1U, channel), options,
            [&](const StreamEvent& event) {
                std::lock_guard<std::mutex> lock(mutex);
                events.push_back(event);
                if (event.kind == deepwin_market_data::kDatagramEvent) {
                    datagram_times.push_back(event.monotonic_ns);
                } else {
                    idle_times.push_back(event.monotonic_ns);
                    idles.store(static_cast<unsigned>(idle_times.size()),
                                std::memory_order_release);
                    if (idle_times.size() == 2U) stream.stop();
                }
            }, 300, &error, [&]() {
                polls.fetch_add(1U, std::memory_order_relaxed);
            });
    });

    for (unsigned i = 0U; i < 300U && !stream.ready(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(stream.ready());
    assert(wait_for_count(polls, 1U, 100U));

    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(sender >= 0);
    for (unsigned value = 1U; value <= 5U; ++value)
        send_packet(sender, channel, static_cast<unsigned char>(value));
    assert(wait_for_count(idles, 1U, 100U));
    send_packet(sender, channel, 2U);
    ::close(sender);
    runner.join();

    assert(ok);
    assert(error.empty());
    assert(polls.load(std::memory_order_relaxed) != 0U);
    assert(idles.load(std::memory_order_relaxed) == 2U);
    assert(events.size() == 8U);
    for (std::size_t i = 0U; i < 5U; ++i) {
        assert(events[i].kind == deepwin_market_data::kDatagramEvent);
        assert(events[i].batch_size >= 1U &&
               events[i].batch_size <= options.receive_batch_size);
        assert(events[i].batch_index < events[i].batch_size);
    }
    assert(events[5].kind == deepwin_market_data::kIdleEvent);
    assert(events[6].kind == deepwin_market_data::kDatagramEvent);
    assert(events[7].kind == deepwin_market_data::kIdleEvent);
    assert(datagram_times.size() == 6U && idle_times.size() == 2U);
    assert(idle_times[0] > datagram_times[4] + options.idle_gap_ns);
    assert(idle_times[1] > datagram_times[5] + options.idle_gap_ns);

    // An owner-poll failure uses the regular processing-failed shutdown path.
    MarketDataStream failed_stream;
    std::atomic<bool> failed_ready(false);
    bool failed_ok = true;
    std::string failed_error;
    std::thread failed_runner([&]() {
        failed_ok = failed_stream.run(std::vector<ChannelSpec>(1U, loopback_channel()),
            options, [](const StreamEvent&) {}, 300, &failed_error, [&]() {
                throw std::runtime_error("poll sentinel");
            });
        failed_ready.store(true, std::memory_order_release);
    });
    assert(wait_for(failed_ready, 300U));
    failed_runner.join();
    assert(!failed_ok);
    assert(failed_error.find("owner poll") != std::string::npos);
    assert(!failed_stream.health().processing_valid);
    std::cout << "market_data_stream_wait_test: PASS polls=" << polls.load()
              << " idle_gap_ns=" << options.idle_gap_ns << "\n";
    return 0;
}
