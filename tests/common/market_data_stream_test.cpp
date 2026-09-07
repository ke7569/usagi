#include "common/stream/MarketDataStream.h"
#include "common/stream/UdpChannelRuntime.h"
#include "sse/sampling/sse_batch_end_sampler.h"

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>

namespace {
using namespace deepwin_market_data;

void require(bool value, const std::string& message) {
    if (!value) { std::cerr << "market_data_stream_test: " << message << '\n'; std::exit(1); }
}

std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        char name[] = "/tmp/t0-stream-test-XXXXXX";
        char* made = ::mkdtemp(name);
        require(made != 0, "mkdtemp");
        path = made;
    }
    ~TemporaryDirectory() {
        clear(path);
        ::rmdir(path.c_str());
    }
    std::string path;
private:
    static void clear(const std::string& directory) {
        DIR* dir = ::opendir(directory.c_str());
        if (!dir) return;
        while (dirent* entry = ::readdir(dir)) {
            if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
            const std::string file = directory + "/" + entry->d_name;
            struct stat info;
            if (::lstat(file.c_str(), &info)) continue;
            if (S_ISDIR(info.st_mode)) { clear(file); ::rmdir(file.c_str()); }
            else ::unlink(file.c_str());
        }
        ::closedir(dir);
    }
};

ChannelSpec channel(const std::string& name) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    require(fd >= 0, "port allocation socket");
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve port");
    socklen_t length = sizeof(address);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0, "get port");
    ::close(fd);
    ChannelSpec result;
    result.name = name;
    result.group = "127.0.0.1";
    result.interface_ip = "127.0.0.1";
    result.port = ntohs(address.sin_port);
    return result;
}

void send(int socket, const ChannelSpec& channel, const std::vector<unsigned char>& bytes) {
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(channel.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::sendto(socket, bytes.data(), bytes.size(), 0,
                     reinterpret_cast<sockaddr*>(&address), sizeof(address)) == static_cast<ssize_t>(bytes.size()),
            "loopback send");
}

struct Saved {
    StreamEvent event;
    std::vector<unsigned char> bytes;
    explicit Saved(const StreamEvent& value) : event(value) {
        if (value.size) bytes.assign(value.data, value.data + value.size);
        event.data = 0;
    }
    bool operator==(const Saved& other) const {
        return std::tie(event.kind, event.sequence, event.monotonic_ns, event.realtime_ns,
                        event.receive_batch, event.channel_id, event.batch_index, event.batch_size,
                        event.source_ipv4, event.source_port, event.timestamp_flags, event.size, bytes) ==
               std::tie(other.event.kind, other.event.sequence, other.event.monotonic_ns, other.event.realtime_ns,
                        other.event.receive_batch, other.event.channel_id, other.event.batch_index, other.event.batch_size,
                        other.event.source_ipv4, other.event.source_port, other.event.timestamp_flags, other.event.size, other.bytes);
    }
};

struct Processing {
    sse_live_sampling::BatchEndSampler sampler;
    std::vector<std::uint64_t> samples;
    std::vector<Saved> events;
    Processing() : sampler(std::vector<std::string>{"600000", "600001"}) {}
    void closed(const std::vector<sse_live_sampling::BatchEnd>& batches) {
        for (const auto& batch : batches) {
            samples.push_back(batch.emitted_ns);
            for (const sse_live_sampling::Candidate& candidate : batch.candidates)
                samples.push_back(candidate.cut_index);
        }
    }
    void on_event(const StreamEvent& event) {
        events.push_back(Saved(event));
        std::vector<sse_live_sampling::BatchEnd> batches;
        std::string error;
        if (event.kind == kIdleEvent) {
            require(sampler.on_timer(event.monotonic_ns, &batches, &error), error);
            closed(batches);
        } else {
            require(sampler.advance_to_event(event.monotonic_ns, &batches, &error), error);
            closed(batches);
            sse_live_sampling::Candidate candidate;
            candidate.instrument_id = event.channel_id == 0 ? "600000" : "600001";
            candidate.cut_index = event.sequence;
            candidate.source_sequence = event.sequence;
            candidate.local_receive_ns = event.monotonic_ns;
            candidate.exchange_time_of_day_micros = 34200000000ULL;
            require(sampler.commit_applied_event(candidate.instrument_id, &candidate, true, &error), error);
        }
    }
};

void test_burst_record_replay() {
    TemporaryDirectory temp;
    StreamOptions options;
    options.recording_directory = temp.path + "/capture";
    options.queue_capacity = 4096;
    options.max_datagram_bytes = 1024;
    options.segment_bytes = 32768;
    options.flush_interval_ms = 10;
    std::vector<ChannelSpec> channels = {channel("tick"), channel("snapshot")};
    MarketDataStream stream;
    Processing live;
    std::atomic<bool> ready(false), finished(false);
    std::vector<std::uint64_t> dispatch_lag;
    bool ok = false;
    std::string error;
    std::thread runner([&]() {
        ok = stream.run(channels, options, [&](const StreamEvent& event) {
            if (event.kind == kDatagramEvent) {
                ready.store(true);
                dispatch_lag.push_back(now_ns() - event.monotonic_ns);
                if (event.size == 440) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            live.on_event(event);
        }, 500, &error);
        finished.store(true);
    });
    const int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    require(socket >= 0, "sender socket");
    std::vector<unsigned char> handshake(1, 7);
    for (int i = 0; i < 300 && !ready.load() && !finished.load(); ++i) {
        send(socket, channels[0], handshake);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(ready.load(), "capture did not become ready");
    std::vector<unsigned char> snapshot(440);
    for (std::size_t i = 0; i < snapshot.size(); ++i) snapshot[i] = static_cast<unsigned char>(i);
    send(socket, channels[1], snapshot);
    std::vector<unsigned char> tick(72);
    for (std::uint32_t index = 0; index < 1000; ++index) {
        for (std::size_t i = 0; i < tick.size(); ++i) tick[i] = static_cast<unsigned char>(index + i);
        std::memcpy(tick.data(), &index, sizeof(index));
        send(socket, channels[0], tick);
    }
    ::close(socket);
    runner.join();
    require(ok, error);
    require(stream.stats().clean_recording, "recording not clean");
    require(stream.stats().written_sequence == live.events.size(), "recording lost events");
    require(stream.stats().durable_sequence == stream.stats().written_sequence, "durable watermark incorrect");
    require(!stream.stats().kernel_drops && !stream.stats().ingress_overflows && !stream.stats().recording_overflows,
            "capture overflow");
    std::uint32_t tick_count = 0;
    std::size_t snapshots = 0, idles = 0;
    for (const Saved& saved : live.events) {
        if (saved.event.kind == kIdleEvent) { ++idles; continue; }
        if (saved.bytes.size() == 72) {
            std::uint32_t index;
            std::memcpy(&index, saved.bytes.data(), sizeof(index));
            require(index == tick_count++, "same-channel burst ordering");
            for (std::size_t i = sizeof(index); i < saved.bytes.size(); ++i)
                require(saved.bytes[i] == static_cast<unsigned char>(index + i), "full tick payload mismatch");
        } else if (saved.bytes.size() == 440) {
            require(saved.bytes == snapshot, "full snapshot payload mismatch");
            ++snapshots;
        }
    }
    require(tick_count == 1000 && snapshots == 1 && idles > 0, "missing burst, snapshot or idle events");
    struct stat info;
    require(::stat((options.recording_directory + "/stream_000001.t0md").c_str(), &info) == 0, "rotation not exercised");
    MarketDataStream replay;
    Processing offline;
    require(replay.replay(options.recording_directory, [&](const StreamEvent& event) {
        if (event.kind == kDatagramEvent && event.sequence % 100 == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        offline.on_event(event);
    }, &error), error);
    require(live.events == offline.events, "live/replay bytes or metadata diverged");
    require(live.samples == offline.samples && !live.samples.empty(), "same sampler diverged under replay delay");
    std::size_t mismatched_callbacks = 0;
    require(!replay.replay(options.recording_directory, [&](const StreamEvent&) { ++mismatched_callbacks; },
                           &error, 0), "different recording idle contract accepted");
    require(mismatched_callbacks == 0 && error.find("idle gap") != std::string::npos,
            "idle contract was not checked before application callbacks");
    std::sort(dispatch_lag.begin(), dispatch_lag.end());
    std::cout << "burst=1000 full_payload=1 rotated=1 sampling_parity=1 synthetic=1"
              << " receive_batches=" << stream.stats().receive_batches
              << " ingress_high_water=" << stream.stats().ingress_high_water
              << " recording_high_water=" << stream.stats().recording_high_water
              << " dispatch_lag_p50_ns=" << dispatch_lag[dispatch_lag.size() / 2]
              << " dispatch_lag_p99_ns=" << dispatch_lag[dispatch_lag.size() * 99 / 100]
              << " dispatch_lag_max_ns=" << dispatch_lag.back() << '\n';

    // Corruption is checked before dispatching any application event.
    const std::string first = options.recording_directory + "/stream_000000.t0md";
    std::fstream corrupt(first.c_str(), std::ios::in | std::ios::out | std::ios::binary);
    corrupt.seekg(64 + 2 * 104 + 72);
    char value;
    corrupt.read(&value, 1);
    value ^= 1;
    corrupt.seekp(64 + 2 * 104 + 72);
    corrupt.write(&value, 1);
    corrupt.close();
    std::size_t callbacks = 0;
    require(!replay.replay(options.recording_directory, [&](const StreamEvent&) { ++callbacks; }, &error),
            "corruption accepted");
    require(callbacks == 0 && error.find("checksum") != std::string::npos, "corrupt preflight did not fail before callback");
}

void test_failure_modes(bool required) {
    TemporaryDirectory temp;
    StreamOptions options;
    options.queue_capacity = 2;
    options.max_datagram_bytes = 128;
    options.recording_directory = temp.path + "/overflow";
    options.recording_required = required;
    const std::vector<ChannelSpec> channels = {channel("tick")};
    MarketDataStream stream;
    std::atomic<bool> ready(false), done(false);
    bool ok = true;
    std::string error;
    std::thread runner([&]() {
        ok = stream.run(channels, options, [&](const StreamEvent& event) {
            if (event.kind == kDatagramEvent && !ready.exchange(true))
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }, 500, &error);
        done.store(true);
    });
    const int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    std::vector<unsigned char> payload(72, 1);
    for (int i = 0; i < 300 && !ready.load() && !done.load(); ++i) {
        send(socket, channels[0], payload);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(ready.load(), "overflow receiver did not become ready");
    for (int i = 0; i < 100; ++i) send(socket, channels[0], payload);
    ::close(socket);
    runner.join();
    require(!ok && stream.stats().ingress_overflows == 1 && !stream.stats().clean_recording,
            "overflow must fail closed, " + error);
    require(!stream.health().input_valid && !stream.health().permits_new_risk(),
            "incomplete input must deny new risk regardless of recording policy");
    MarketDataStream replay;
    std::size_t callbacks = 0;
    require(!replay.replay(options.recording_directory, [&](const StreamEvent&) { ++callbacks; }, &error),
            "incomplete recording accepted");
    require(callbacks == 0, "incomplete replay dispatched events");
    options.recording_required = true;
    require(!stream.run(channels, options, [](const StreamEvent&) {}, 1, &error), "existing recording overwritten");
    require(stream.health().input_valid && stream.health().recording_failed && !stream.health().permits_new_risk(),
            "recording setup error must remain separate from invalid input");
    options.recording_directory.clear();
    require(!stream.run(channels, options, [](const StreamEvent&) {}, 1, &error),
            "required recording silently disabled by empty directory");
    require(!stream.health().permits_new_risk(), "invalid recording configuration permitted new risk");
    options.recording_required = false;
    options.queue_capacity = 0;
    require(!stream.run(channels, options, [](const StreamEvent&) {}, 1, &error), "invalid queue capacity accepted");
}

void test_strict_gap_is_not_receive_batch() {
    Processing processor;
    unsigned char payload = 1;
    StreamEvent event = {};
    event.kind = kDatagramEvent;
    event.data = &payload;
    event.size = 1;
    event.monotonic_ns = 1000000;
    event.sequence = 1;
    event.receive_batch = 1;
    processor.on_event(event);
    event.sequence = 2;
    event.receive_batch = 2;
    event.monotonic_ns += 100000;
    processor.on_event(event);
    require(processor.samples.empty(), "receive batch or exact 100us created a model sample");
    event.kind = kIdleEvent;
    event.data = 0;
    event.size = 0;
    event.monotonic_ns += 100001;
    event.sequence = 3;
    processor.on_event(event);
    require(processor.samples.size() == 2 && processor.samples.back() == 2,
            "strict >100us did not emit only the final dirty candidate");
}

}  // namespace

int main() {
    test_strict_gap_is_not_receive_batch();
    test_burst_record_replay();
    test_failure_modes(true);
    test_failure_modes(false);
    std::cout << "market_data_stream_test: PASS\n";
    return 0;
}
