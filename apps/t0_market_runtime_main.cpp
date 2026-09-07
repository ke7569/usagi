#include "common/contracts/MarketRuntimeApi.h"
#include "common/config/StreamConfigJson.h"
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <pthread.h>

namespace {
struct Handle {
    const T0MarketRuntimeApiV1* api;
    void* value;
    ~Handle() { api->destroy(value); }
};

nlohmann::json status(const Handle& handle) {
    // Status can grow while the worker finishes between the size/read calls.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::size_t size = handle.api->status_json(handle.value, 0, 0);
        if (!size) throw std::runtime_error("runtime status unavailable");
        std::vector<char> buffer(size);
        const std::size_t required = handle.api->status_json(handle.value, buffer.data(), buffer.size());
        if (required && required <= buffer.size()) return nlohmann::json::parse(buffer.data());
    }
    throw std::runtime_error("runtime status did not stabilize");
}
}

int main(int argc, char** argv) try {
    if (argc != 4) throw std::runtime_error(
        "usage: market-stream capture STREAM.json PROFILE.json | replay DIRECTORY PROFILE.json | recovery-journal DIRECTORY PROFILE.json | recovery-handoff DIRECTORY PROFILE.json");
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &signals, 0)) throw std::runtime_error("cannot block stop signals");
    const T0MarketRuntimeApiV1* api = t0_market_runtime_v1();
    char error[4096] = {};
    Handle handle = {api, api->create(argv[1], argv[2], argv[3], error, sizeof(error))};
    if (!handle.value || !api->start(handle.value, error, sizeof(error))) throw std::runtime_error(error);
    bool ready_announced = false, recording_failure_announced = false;
    for (;;) {
        const nlohmann::json current = status(handle);
        if (current.value("done", false)) break;
        if (!ready_announced && current.value("ready", false)) {
            std::cerr << "market-data stream ready\n";
            ready_announced = true;
        }
        if (!recording_failure_announced && current.count("recording_error")) {
            std::cerr << "recording degraded: " << current.at("recording_error").get<std::string>() << '\n';
            recording_failure_announced = true;
        }
        timespec wait = {0, 1000000L};
        const int signal = ::sigtimedwait(&signals, 0, &wait);
        if (signal == SIGINT || signal == SIGTERM) api->request_stop(handle.value);
    }
    const bool ok = api->join(handle.value, error, sizeof(error)) != 0;
    std::cout << status(handle).dump() << '\n';
    if (!ok) std::cerr << error << '\n';
    return ok ? 0 : 1;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
