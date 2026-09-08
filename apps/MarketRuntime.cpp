#include "common/contracts/MarketRuntimeApi.h"
#include "common/config/StreamInputConfig.h"
#include "apps/StreamProcessingCli.h"
#ifdef T0_STREAM_SSE
#include "sse/runtime/sse_cpu_affinity.h"
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace {
typedef nlohmann::json Json;

void message(const std::string& value, char* output, std::size_t capacity) {
    if (!output || !capacity) return;
    const std::size_t size = std::min(capacity - 1, value.size());
    std::memcpy(output, value.data(), size);
    output[size] = 0;
}

class Runtime {
public:
    Runtime(const std::string& action, const std::string& input, const std::string& profile)
        : action_(action), input_(input), duration_(0), started_(false), stopped_(false), done_(false),
          succeeded_(false), datagrams_(0), idles_(0), payload_bytes_(0) {
        const bool capture = action == "capture";
        const bool handoff = action == "recovery-handoff";
        const bool journal = action == "recovery-journal";
        if (!capture && !handoff && !journal && action != "replay")
            throw std::runtime_error("unknown runtime input action");
#ifndef T0_STREAM_SZE
        if (handoff || journal) throw std::runtime_error("SSE runtime does not support SZE recovery");
#endif
        if (capture) stream_input::load(input, &channels_, &options_, &duration_);
#ifdef T0_STREAM_SSE
        if (capture && options_.idle_gap_ns != 100000)
            throw std::runtime_error("SSE per-instrument-v2 requires idle_gap_ns=100000");
#endif
        const std::string driver = handoff ? "sze-handoff" : journal ? "sze-journal" : "raw";
        application_.reset(new StreamProcessingCli(profile, capture || handoff,
            capture ? options_.recording_directory : input, capture ? channels_.size() : 64,
            [this, capture]() {
                if (stopped_.load()) return false;
                if ((capture || action_ == "replay") && stream_.stopping()) return false;
                return input_health().permits_new_risk();
            }, driver));
    }

    ~Runtime() {
        request_stop();
        (void)join();
    }

    void start() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (started_.load()) throw std::runtime_error("runtime is single-start");
        if (stopped_.load()) throw std::runtime_error("runtime stopped before start");
        started_.store(true);
        try {
            // MarketDataStream is reusable and resets its own stop flag on run.
            // Latch the owner's stop across that startup boundary. This worker
            // sleeps until stop/finish; it does not poll during normal operation.
            stop_worker_ = std::thread([this]() {
                std::unique_lock<std::mutex> lock(stop_mutex_);
                wake_.wait(lock, [this]() { return stopped_.load() || done_.load(); });
                while (!done_.load()) {
                    stream_.stop();
                    wake_.wait_for(lock, std::chrono::milliseconds(1));
                }
            });
            worker_ = std::thread([this]() { run(); });
        } catch (const std::exception& error) { fail_start(error.what()); throw; }
          catch (...) { fail_start("unknown runtime thread creation failure"); throw; }
    }

    void request_stop() {
        if (application_) application_->begin_stop();
        stopped_.store(true);
        stream_.stop();
        notify_stop_worker();
    }

    bool join() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!started_.load()) return stopped_.load();
        if (worker_.joinable()) worker_.join();
        if (stop_worker_.joinable()) stop_worker_.join();
        return succeeded_;
    }

    std::string error() const { return done_.load() ? error_ : std::string("runtime has not completed"); }

    std::string status() const {
        const bool done = done_.load();
        if (done) return result_.empty() ? Json({{"done", true}, {"state", "failed"}, {"ok", false}}).dump() : result_;
        Json result;
        result["done"] = !started_.load() && stopped_.load();
        result["state"] = stopped_.load() ? "stopped" : started_.load() ? "running" : "created";
        result["ready"] = !stopped_.load() && (stream_.ready() || application_->recovery_ready());
        if (stream_.health().recording_failed) result["recording_error"] = stream_.recording_error();
        return result.dump();
    }

private:
    deepwin_market_data::StreamHealth input_health() const {
        if (action_ == "capture" || action_ == "replay") return stream_.health();
        deepwin_market_data::StreamHealth health = {
            application_ && application_->recovery_available(),
            application_ && application_->processing_valid(), false, false};
        return health;
    }

    void fail_start(const std::string& error) {
        application_->begin_stop();
        stopped_.store(true);
        error_ = error;
        result_ = Json({{"done", true}, {"state", "failed"}, {"ok", false},
            {"ready", false}, {"error", error_}, {"processing", application_->summary()}}).dump();
        done_.store(true);
        notify_stop_worker();
        if (stop_worker_.joinable()) stop_worker_.join();
    }

    void notify_stop_worker() {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        wake_.notify_all();
    }

    void run() {
#ifdef T0_STREAM_SSE
        // Keep the lease until receive, dispatch and recording workers have
        // joined. Replay has no concurrent capture workers and needs no lease.
        sse_cpu::Lease cpu_lease;
        Json cpu_affinity = Json::array();
#endif
        try {
            if (stopped_.load()) throw std::runtime_error("runtime stopped during startup");
#ifdef T0_STREAM_SSE
            if (action_ == "capture") {
                if (!cpu_lease.acquire({options_.receive_cpu, options_.dispatch_cpu,
                                       options_.writer_cpu}, &error_))
                    throw std::runtime_error(error_);
                const std::vector<sse_cpu::Cpu>& cpus = cpu_lease.cpus();
                options_.receive_cpu = cpus[0].id;
                options_.dispatch_cpu = cpus[1].id;
                options_.writer_cpu = cpus[2].id;
                const char* roles[] = {"receive", "dispatch", "writer"};
                for (std::size_t i = 0; i < cpus.size(); ++i)
                    cpu_affinity.push_back({{"role", roles[i]}, {"cpu", cpus[i].id}, {"l3", cpus[i].l3}});
            }
#endif
            if (action_ == "recovery-journal" || action_ == "recovery-handoff") {
                succeeded_ = application_->run_recovery();
                if (!succeeded_) error_ = application_->recovery_error();
            } else {
                const deepwin_market_data::StreamCallback callback = [this](const deepwin_market_data::StreamEvent& event) {
                    if (event.kind == deepwin_market_data::kDatagramEvent) {
                        ++datagrams_; payload_bytes_ += event.size;
                    } else ++idles_;
                    application_->on_event(event);
                };
                if (action_ == "capture") {
#ifdef T0_STREAM_SSE
                    // run dispatches on this owner thread. Model completions
                    // must also be drained while the ingress queue is empty.
                    succeeded_ = stream_.run(channels_, options_, callback, duration_, &error_,
                        [this]() { application_->poll_outputs(); });
#else
                    succeeded_ = stream_.run(channels_, options_, callback, duration_, &error_);
#endif
                }
                else {
#ifdef T0_STREAM_SSE
                    succeeded_ = stream_.replay(input_, callback, &error_, 100000);
#else
                    succeeded_ = stream_.replay(input_, callback, &error_);
#endif
                }
            }
        } catch (const std::exception& exception) { error_ = exception.what(); succeeded_ = false; }
          catch (...) { error_ = "unknown market runtime error"; succeeded_ = false; }
#ifdef T0_STREAM_SSE
        // Same thread as all on_event/poll_outputs callbacks, after raw input
        // stopped. Finish only work sealed by a recorded BatchEnd; no EOF cut.
        try { application_->finish(); }
        catch (const std::exception& exception) {
            if (error_.empty()) error_ = exception.what();
            succeeded_ = false;
        } catch (...) {
            if (error_.empty()) error_ = "unknown SSE processing drain error";
            succeeded_ = false;
        }
#endif
        application_->begin_stop();
        try {
            Json out;
            out["ok"] = succeeded_;
            out["done"] = true;
            out["state"] = !succeeded_ ? "failed" : stopped_.load() ? "stopped" : "completed";
            out["ready"] = false;
            out["stop_requested"] = stopped_.load();
#ifdef T0_STREAM_SSE
            out["cpu_affinity"] = cpu_affinity;
#endif
            out["processing"] = application_->summary();
            out["datagrams"] = datagrams_; out["idle_events"] = idles_; out["payload_bytes"] = payload_bytes_;
            const deepwin_market_data::StreamStats& stats = stream_.stats();
            out["stats"]["written_sequence"] = stats.written_sequence;
            out["stats"]["durable_sequence"] = stats.durable_sequence;
            out["stats"]["received_datagrams"] = stats.received_datagrams;
            out["stats"]["receive_batches"] = stats.receive_batches;
            out["stats"]["ingress_high_water"] = stats.ingress_high_water;
            out["stats"]["recording_high_water"] = stats.recording_high_water;
            out["stats"]["high_water"] = std::max(stats.ingress_high_water, stats.recording_high_water);
            out["stats"]["overflows"] = stats.ingress_overflows + stats.recording_overflows;
            out["stats"]["kernel_drops"] = stats.kernel_drops;
            out["stats"]["clean_recording"] = stats.clean_recording;
            const deepwin_market_data::StreamHealth health = input_health();
            out["health"] = {{"input_valid", health.input_valid}, {"processing_valid", health.processing_valid},
                {"recording_failed", health.recording_failed}, {"recording_required", health.recording_required},
                {"permits_new_risk", health.permits_new_risk()}};
            if (health.recording_failed) out["recording_error"] = stream_.recording_error();
            if (!succeeded_) out["error"] = error_;
            result_ = out.dump();
        } catch (...) { succeeded_ = false; }
        done_.store(true);
        notify_stop_worker();
    }

    std::string action_, input_;
    deepwin_market_data::MarketDataStream stream_;
    deepwin_market_data::StreamOptions options_;
    std::vector<deepwin_market_data::ChannelSpec> channels_;
    long duration_;
    std::unique_ptr<StreamProcessingCli> application_;
    std::atomic<bool> started_, stopped_, done_;
    bool succeeded_;
    std::uint64_t datagrams_, idles_, payload_bytes_;
    std::thread worker_, stop_worker_;
    std::mutex lifecycle_mutex_, stop_mutex_;
    std::condition_variable wake_;
    std::string error_, result_;
};

void* create(const char* action, const char* input, const char* profile, char* error, std::size_t bytes) {
    message("", error, bytes);
    try {
        if (!action || !input || !profile) throw std::runtime_error("runtime arguments cannot be null");
        return new Runtime(action, input, profile);
    } catch (const std::exception& e) { message(e.what(), error, bytes); }
      catch (...) { message("runtime construction failed", error, bytes); }
    return 0;
}
int start(void* handle, char* error, std::size_t bytes) {
    message("", error, bytes);
    try {
        if (!handle) throw std::runtime_error("null runtime handle");
        static_cast<Runtime*>(handle)->start();
        return 1;
    } catch (const std::exception& e) { message(e.what(), error, bytes); }
      catch (...) { message("runtime start failed", error, bytes); }
    return 0;
}
void request_stop(void* handle) { if (handle) static_cast<Runtime*>(handle)->request_stop(); }
int join(void* handle, char* error, std::size_t bytes) {
    message("", error, bytes);
    try {
        if (!handle) throw std::runtime_error("null runtime handle");
        if (static_cast<Runtime*>(handle)->join()) return 1;
        message(static_cast<Runtime*>(handle)->error(), error, bytes);
    } catch (const std::exception& e) { message(e.what(), error, bytes); }
      catch (...) { message("runtime join failed", error, bytes); }
    return 0;
}
std::size_t status(void* handle, char* output, std::size_t bytes) {
    if (output && bytes) output[0] = 0;
    try {
        if (!handle) return 0;
        const std::string value = static_cast<Runtime*>(handle)->status();
        if (output && bytes > value.size()) message(value, output, bytes);
        return value.size() + 1;
    } catch (...) { return 0; }
}
void destroy(void* handle) { delete static_cast<Runtime*>(handle); }
}

extern "C" __attribute__((visibility("default"))) const T0MarketRuntimeApiV1* t0_market_runtime_v1() {
#ifdef T0_STREAM_SZE
    static const char* market = "SZ";
#else
    static const char* market = "SH";
#endif
    static const T0MarketRuntimeApiV1 api = {1, sizeof(T0MarketRuntimeApiV1), market,
        create, start, request_stop, join, status, destroy};
    return &api;
}
