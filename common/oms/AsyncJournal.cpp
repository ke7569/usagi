#include "common/oms/AsyncJournal.h"
#include "common/oms/Profile.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <cstdlib>
#include <sched.h>

namespace oms {
AsyncJournal::AsyncJournal(const std::string& path, std::size_t capacity, const Sink& sink)
    : file_(path), sink_(path.empty() ? sink : Sink()), records_((!path.empty() || sink) && capacity <= 65536 ? capacity : 0),
      head_(0), tail_(0), written_(0), durable_(0), sync_target_(0), queued_bytes_(0),
      accepted_(0), failed_(false), stopping_(false), replayed_(false) {
    if (!capacity || capacity > 65536) throw std::invalid_argument("invalid OMS journal queue capacity");
    if (!path.empty() && sink) sink_writer_.reset(new AsyncJournal(std::string(), capacity, sink));
    if (!file_.healthy()) fail(file_.error());
}
AsyncJournal::~AsyncJournal() {
    stopping_.store(true, std::memory_order_release); wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}
bool AsyncJournal::fail(const std::string& message) {
    std::lock_guard<std::mutex> guard(error_mutex_);
    if (!failed_.load(std::memory_order_relaxed)) {
        error_ = message; failed_.store(true, std::memory_order_release);
    }
    return false;
}
std::string AsyncJournal::error() const {
    if (sink_writer_ && !sink_writer_->healthy()) return sink_writer_->error();
    std::lock_guard<std::mutex> guard(error_mutex_); return error_;
}
std::size_t AsyncJournal::pending() const {
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    return std::min(records_.size(), static_cast<std::size_t>(head_.load(std::memory_order_acquire) - tail));
}
bool AsyncJournal::replay(const std::function<bool(const std::string&)>& visitor) {
    if (replayed_) return healthy();
    if (!healthy()) return false;
    const bool ok = file_.replay(visitor);
    accepted_ = file_.sequence(); written_.store(accepted_); durable_.store(file_.durable_sequence());
    if (!ok) return fail(file_.error());
    replayed_ = true;
    if (sink_writer_ && !sink_writer_->replay(std::function<bool(const std::string&)>())) return fail(sink_writer_->error());
    if (recording()) worker_ = std::thread(&AsyncJournal::run, this);
    return true;
}
bool AsyncJournal::append(std::string payload, bool synchronize) {
    if (!healthy()) return false;
    if (!replayed_ || stopping_.load()) return fail("journal is not accepting records");
    if (!recording()) {
        if (!file_.append(payload, synchronize)) return fail(file_.error());
        accepted_ = file_.sequence(); written_.store(accepted_); return true;
    }
    OMS_PROFILE_SCOPE(profile_append, JournalAppend);
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (payload.size() > 1048576 || accepted_ == std::numeric_limits<std::uint64_t>::max())
        return fail("journal record exceeds capacity");
    if (payload.size() > 8388608 - queued_bytes_.load(std::memory_order_acquire))
        return fail("journal byte queue exhausted");
    if (head - tail_.load(std::memory_order_acquire) >= records_.size())
        return fail("journal background queue exhausted");
    Record& record = records_[head % records_.size()];
    record.payload = std::move(payload); record.synchronize = synchronize; record.sequence = ++accepted_;
    queued_bytes_.fetch_add(record.payload.size(), std::memory_order_release);
    head_.store(head + 1, std::memory_order_release);
    if (!synchronize || !persistent()) return true;
    wake_.notify_one();
    return wait_for(accepted_);
}
bool AsyncJournal::wait_for(std::uint64_t sequence) {
    OMS_PROFILE_SCOPE(profile_wait, JournalSync);
    std::unique_lock<std::mutex> guard(completion_mutex_);
    while (healthy() && (persistent() ? durable_sequence() : written_sequence()) < sequence)
        completion_.wait_for(guard, std::chrono::milliseconds(1));
    return healthy() && (persistent() ? durable_sequence() : written_sequence()) >= sequence;
}
bool AsyncJournal::sync() {
    if (!healthy() || !replayed_) return false;
    if (!recording()) return file_.sync();
    sync_target_.store(accepted_, std::memory_order_release); wake_.notify_one();
    return wait_for(accepted_);
}
void AsyncJournal::publish() {
    std::lock_guard<std::mutex> guard(completion_mutex_); completion_.notify_all();
}
void AsyncJournal::run() {
    typedef std::chrono::steady_clock Clock;
    Clock::time_point last_sync = Clock::now();
    try {
        const char* cpu_text=std::getenv("SSE_OMS_JOURNAL_CPU");
        if(cpu_text && *cpu_text) {
            char* end=0;long cpu=std::strtol(cpu_text,&end,10);cpu_set_t set;CPU_ZERO(&set);
            if(*end || cpu<0 || cpu>=CPU_SETSIZE)throw std::runtime_error("invalid OMS journal CPU");
            CPU_SET(cpu,&set);if(sched_setaffinity(0,sizeof(set),&set))throw std::runtime_error("OMS journal affinity failed");
        }
        while (healthy()) {
            const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
            if (tail != head_.load(std::memory_order_acquire)) {
                Record& record = records_[tail % records_.size()];
                if (!file_.append(record.payload, record.synchronize)) { fail(file_.error()); break; }
                if (persistent()) written_.store(record.sequence, std::memory_order_release);
                if (record.synchronize) {
                    durable_.store(file_.durable_sequence(), std::memory_order_release); last_sync = Clock::now(); publish();
                }
                if (sink_writer_ && !sink_writer_->append(record.payload, false)) { fail(sink_writer_->error()); break; }
                if (sink_) sink_(record.payload);
                if (!persistent()) written_.store(record.sequence, std::memory_order_release);
                queued_bytes_.fetch_sub(record.payload.size(), std::memory_order_release);
                std::string().swap(record.payload); tail_.store(tail + 1, std::memory_order_release);
                if (!persistent()) publish();
            }
            const bool empty = tail_.load(std::memory_order_relaxed) == head_.load(std::memory_order_acquire);
            const bool requested = sync_target_.load(std::memory_order_acquire) > durable_sequence() &&
                written_sequence() >= sync_target_.load(std::memory_order_acquire);
            if (persistent() && file_.sequence() > file_.durable_sequence() &&
                (requested || (stopping_.load() && empty) || Clock::now() - last_sync >= std::chrono::milliseconds(10))) {
                if (!file_.sync()) { fail(file_.error()); break; }
                durable_.store(file_.durable_sequence(), std::memory_order_release); last_sync = Clock::now(); publish();
            }
            if (empty && stopping_.load(std::memory_order_acquire)) break;
            if (empty) {
                std::unique_lock<std::mutex> guard(wake_mutex_);
                wake_.wait_for(guard, std::chrono::milliseconds(1), [&]() {
                    return stopping_.load() || !healthy() || head_.load() != tail_.load() ||
                        sync_target_.load() > (persistent() ? durable_sequence() : written_sequence());
                });
            }
        }
    } catch (const std::exception& e) { fail(e.what()); }
    catch (...) { fail("journal worker failed"); }
    publish();
}
}  // namespace oms
