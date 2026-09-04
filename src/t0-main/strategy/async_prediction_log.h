#ifndef SSE_T0_ASYNC_PREDICTION_LOG_H
#define SSE_T0_ASYNC_PREDICTION_LOG_H

// Async appended prediction log.  The trading path never writes prediction
// CSVs; this class records prediction lines in the background so the hot
// path only enqueues a string.  One writer thread drains a deque to an
// append-mode file.

#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

namespace sse_trading {

class AsyncPredictionLog {
public:
    AsyncPredictionLog() : running_(false), stop_(false), dropped_(0) {}

    ~AsyncPredictionLog() { close(); }

    bool open(const std::string& path) {
        if (path.empty() || running_) return false;
        out_.open(path.c_str(), std::ios::out | std::ios::app);
        if (!out_.is_open()) return false;
        stop_ = false;
        running_ = true;
        worker_ = std::thread(&AsyncPredictionLog::run, this);
        return true;
    }

    void enqueue(const std::string& line) {
        if (!running_) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= kMaxQueue) { ++dropped_; return; }
            queue_.push_back(line);
        }
        cv_.notify_one();
    }

    void close() {
        if (!running_) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        running_ = false;
        if (out_.is_open()) {
            out_.flush();
            out_.close();
        }
    }

private:
    void run() {
        std::deque<std::string> local;
        std::size_t written_since_flush = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
                if (queue_.empty() && stop_) break;
                local.swap(queue_);
            }
            while (!local.empty()) {
                out_ << local.front() << '\n';
                local.pop_front();
                ++written_since_flush;
            }
            static const std::size_t kFlushBatch = 256;
            if (written_since_flush >= kFlushBatch) {
                out_.flush();
                written_since_flush = 0;
            }
        }
        out_.flush();
    }

    std::ofstream out_;
    std::deque<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool running_;
    bool stop_;
    std::size_t dropped_;
    static const std::size_t kMaxQueue = 16384;
};

}  // namespace sse_trading

#endif  // SSE_T0_ASYNC_PREDICTION_LOG_H
