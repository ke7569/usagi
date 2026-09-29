#ifndef USAGI_ASYNC_DIAGNOSTIC_LOG_H
#define USAGI_ASYNC_DIAGNOSTIC_LOG_H

#include "third_party/nlohmann/json.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <sched.h>

namespace diagnostic_log {

// Multiple callback/OMS producers, one writer. The queue mutex never covers
// formatting, file output or flush. This is diagnostic evidence, not the
// durable OMS journal: failure cannot alter a possibly submitted order.
class Sink {
public:
    explicit Sink(const char* environment) noexcept
        : label_(environment), enabled_(false), stopping_(false), failed_(false),
          lost_(0), capacity_(4096) {
        try {
            const char* path = std::getenv(environment);
            if (!path || !*path) return;
            if (*path != '/') throw std::runtime_error("absolute diagnostic path required");
            file_.exceptions(std::ios::badbit | std::ios::failbit);
            file_.open(path, std::ios::out | std::ios::app);
            writer_ = std::thread(&Sink::run, this);
            enabled_ = true;
        } catch (...) {
            failed_.store(true);
            std::fprintf(stderr, "%s: diagnostic startup failed\n", label_);
        }
    }
    ~Sink() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (writer_.joinable()) writer_.join();
        if (lost_.load()) std::fprintf(stderr, "%s: diagnostic records lost=%llu\n",
                                      label_, (unsigned long long)lost_.load());
    }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    bool enabled() const { return enabled_; }
    typedef std::function<nlohmann::json()> RecordBuilder;
    void push(nlohmann::json&& row) noexcept {
        if(!enabled_)return;
        try {
            auto value=std::make_shared<nlohmann::json>(std::move(row));
            enqueue([value](){return std::move(*value);});
        } catch(...) { lost_.fetch_add(1); }
    }
    void enqueue(RecordBuilder build) noexcept {
        if (!enabled_) return;
        try {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (failed_.load() || stopping_ || queue_.size() >= capacity_) {
                    lost_.fetch_add(1); return;
                }
                queue_.push_back(std::move(build));
            }
            wake_.notify_one();
        } catch (...) { lost_.fetch_add(1); }
    }
    std::uint64_t lost() const { return lost_.load(); }
    // Called only after producers have stopped, before unloading their code.
    void drain() {
        if(!enabled_)return;
        std::unique_lock<std::mutex> lock(mutex_);
        drained_.wait(lock,[&](){return queue_.empty()&&!writing_;});
    }

private:
    void run() noexcept {
        std::deque<RecordBuilder> batch;
        try {
            // Reuse the configured diagnostic CPU; no new dedicated role.
            const char* cpu_text = std::getenv("SSE_ATP_TRACE_CPU");
            if (cpu_text && *cpu_text) {
                char* end = 0;
                const long cpu = std::strtol(cpu_text, &end, 10);
                if (*end || cpu < 0 || cpu >= CPU_SETSIZE)
                    throw std::runtime_error("diagnostic CPU invalid");
                cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu, &mask);
                if (sched_setaffinity(0, sizeof(mask), &mask))
                    throw std::runtime_error("diagnostic CPU affinity");
            }
            for (;;) {
                {
                    std::unique_lock<std::mutex> guard(mutex_);
                    wake_.wait(guard, [&]() { return stopping_ || !queue_.empty(); });
                    if (queue_.empty() && stopping_) break;
                    batch.swap(queue_);writing_=true;
                }
                for (const auto& build : batch) file_ << build().dump() << '\n';
                file_.flush();
                batch.clear();
                {std::lock_guard<std::mutex> guard(mutex_);writing_=false;}
                drained_.notify_all();
                const auto dropped = lost_.load();
                if (dropped != reported_lost_) {
                    std::fprintf(stderr, "%s: diagnostic records lost=%llu\n",
                                 label_, (unsigned long long)dropped);
                    reported_lost_ = dropped;
                }
            }
        } catch (...) {
            failed_.store(true);
            std::lock_guard<std::mutex> guard(mutex_);
            lost_.fetch_add(batch.size() + queue_.size());
            batch.clear();queue_.clear();writing_=false;drained_.notify_all();
            std::fprintf(stderr, "%s: diagnostic writer failed; evidence may be incomplete\n", label_);
        }
    }

    const char* label_;
    bool enabled_, stopping_;
    std::atomic<bool> failed_;
    std::atomic<std::uint64_t> lost_;
    std::uint64_t reported_lost_ = 0;
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable wake_, drained_;
    bool writing_=false;
    std::deque<RecordBuilder> queue_;
    std::ofstream file_;
    std::thread writer_;
};
}
#endif
