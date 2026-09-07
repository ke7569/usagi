#ifndef USAGI_OMS_ASYNC_JOURNAL_H
#define USAGI_OMS_ASYNC_JOURNAL_H

#include "common/oms/Journal.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

namespace oms {
// One externally serialized producer; disk IO belongs exclusively to the worker.
class AsyncJournal {
public:
    typedef std::function<void(const std::string&)> Sink;
    explicit AsyncJournal(const std::string& path, std::size_t capacity = 8192, const Sink& sink = Sink());
    ~AsyncJournal();
    bool replay(const std::function<bool(const std::string&)>& visitor);
    bool append(std::string payload, bool synchronize);
    bool sync();
    bool healthy() const {
        return !failed_.load(std::memory_order_acquire) && (!sink_writer_ || sink_writer_->healthy());
    }
    bool persistent() const { return file_.persistent(); }
    bool recording() const { return persistent() || static_cast<bool>(sink_); }
    std::string error() const;
    std::uint64_t sequence() const { return accepted_; }
    std::uint64_t written_sequence() const { return written_.load(std::memory_order_acquire); }
    std::uint64_t durable_sequence() const { return durable_.load(std::memory_order_acquire); }
    std::size_t pending() const;
    std::size_t sink_pending() const { return sink_writer_ ? sink_writer_->pending() : (sink_ ? pending() : 0); }
private:
    struct Record { std::string payload; bool synchronize = false; std::uint64_t sequence = 0; };
    bool fail(const std::string& message);
    bool wait_for(std::uint64_t sequence);
    void publish();
    void run();
    Journal file_;
    Sink sink_;
    std::unique_ptr<AsyncJournal> sink_writer_;
    std::vector<Record> records_;
    std::atomic<std::uint64_t> head_, tail_, written_, durable_, sync_target_;
    std::atomic<std::size_t> queued_bytes_;
    std::uint64_t accepted_;
    std::atomic<bool> failed_, stopping_;
    bool replayed_;
    mutable std::mutex error_mutex_;
    std::string error_;
    std::mutex wake_mutex_, completion_mutex_;
    std::condition_variable wake_, completion_;
    std::thread worker_;
};
}  // namespace oms
#endif
