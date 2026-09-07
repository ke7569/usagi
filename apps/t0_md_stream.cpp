#include "common/stream/MarketDataStream.h"
#include "common/stream/UdpChannelRuntime.h"
#include "common/config/StreamInputConfig.h"

#include <climits>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <pthread.h>

namespace {

typedef nlohmann::json Json;

int fail(const std::string& error) { std::cerr << error << "\n"; return 1; }

class StopSignals {
public:
    explicit StopSignals(deepwin_market_data::MarketDataStream* stream) : done_(false) {
        sigemptyset(&signals_);
        sigaddset(&signals_, SIGINT);
        sigaddset(&signals_, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &signals_, &old_) != 0)
            throw std::runtime_error("cannot configure stop signals");
        try {
            thread_ = std::thread([this, stream]() {
                bool requested = false;
                bool announced_ready = false;
                bool announced_recording_failure = false;
                while (!done_.load()) {
                    timespec wait = {0, 10000000L};
                    const int signal = sigtimedwait(&signals_, 0, &wait);
                    if (signal == SIGINT || signal == SIGTERM) requested = true;
                    if (requested) {
                        stream->stop();
                    }
                    if (!announced_ready && stream->ready()) {
                        std::cerr << "market-data stream ready\n";
                        announced_ready = true;
                    }
                    if (!announced_recording_failure && stream->health().recording_failed) {
                        std::cerr << "recording degraded: " << stream->recording_error() << "\n";
                        announced_recording_failure = true;
                    }
                }
            });
        } catch (...) {
            pthread_sigmask(SIG_SETMASK, &old_, 0);
            throw;
        }
    }
    ~StopSignals() {
        done_.store(true);
        thread_.join();
        pthread_sigmask(SIG_SETMASK, &old_, 0);
    }
private:
    sigset_t signals_, old_;
    std::atomic<bool> done_;
    std::thread thread_;
};

} // namespace

int main(int argc, char** argv) try {
    if (argc != 3 || (std::string(argv[1]) != "capture" && std::string(argv[1]) != "replay"))
        return fail("usage: t0_md_stream capture CONFIG.json | replay DIRECTORY");
    deepwin_market_data::MarketDataStream stream;
    std::uint64_t datagrams = 0, idles = 0, payload_bytes = 0;
    deepwin_market_data::StreamCallback callback = [&](const deepwin_market_data::StreamEvent& event) {
        if (event.kind == deepwin_market_data::kDatagramEvent) { ++datagrams; payload_bytes += static_cast<std::uint64_t>(event.size); }
        else if (event.kind == deepwin_market_data::kIdleEvent) ++idles;
    };
    std::string error;
    bool ok = false;
    if (std::string(argv[1]) == "capture") {
        std::vector<deepwin_market_data::ChannelSpec> channels;
        deepwin_market_data::StreamOptions options;
        long duration = 0;
        stream_input::load(argv[2], &channels, &options, &duration);
        StopSignals stop_signals(&stream);
        ok = stream.run(channels, options, callback, duration, &error);
    } else {
        StopSignals stop_signals(&stream);
        ok = stream.replay(argv[2], callback, &error);
    }
    const deepwin_market_data::StreamStats& s = stream.stats();
    Json out;
    out["ok"] = ok;
    out["datagrams"] = datagrams; out["idle_events"] = idles; out["payload_bytes"] = payload_bytes;
    out["stats"]["written_sequence"] = s.written_sequence;
    out["stats"]["durable_sequence"] = s.durable_sequence;
    out["stats"]["received_datagrams"] = s.received_datagrams;
    out["stats"]["receive_batches"] = s.receive_batches;
    out["stats"]["ingress_high_water"] = s.ingress_high_water;
    out["stats"]["recording_high_water"] = s.recording_high_water;
    out["stats"]["high_water"] = s.ingress_high_water > s.recording_high_water ? s.ingress_high_water : s.recording_high_water;
    out["stats"]["overflows"] = s.ingress_overflows + s.recording_overflows;
    out["stats"]["kernel_drops"] = s.kernel_drops;
    out["stats"]["clean_recording"] = s.clean_recording;
    const deepwin_market_data::StreamHealth health = stream.health();
    out["health"]["input_valid"] = health.input_valid;
    out["health"]["processing_valid"] = health.processing_valid;
    out["health"]["recording_failed"] = health.recording_failed;
    out["health"]["recording_required"] = health.recording_required;
    out["health"]["permits_new_risk"] = health.permits_new_risk();
    if (health.recording_failed) {
        out["recording_error"] = stream.recording_error();
        std::cerr << "recording degraded: " << stream.recording_error() << "\n";
    }
    if (!ok) {
        const std::string message = error.empty() ? "market data stream failed" : error;
        std::cerr << message << "\n";
        out["error"] = message;
    }
    std::cout << out.dump() << "\n";
    return ok ? 0 : 1;
} catch (const std::exception& exception) {
    return fail(exception.what());
}
