#ifndef SSE_SHM_PREFETCH_H
#define SSE_SHM_PREFETCH_H

#include "common/recovery/SZERecoverable.h"
#include <atomic>
#include <memory>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <time.h>

namespace sse_journal {

// One reader validates and copies SHM records. The application borrows a slot
// until its next read; a full queue applies backpressure without overwriting it.
// The existing ring mapping must outlive this reader (stop before closing it).
class ShmPrefetch {
public:
    static void* operator new(std::size_t size) {
        void* p=nullptr;if(posix_memalign(&p,64,size))throw std::bad_alloc();return p;
    }
    static void operator delete(void* p) {std::free(p);}
    struct Record {
        sze_recovery::CanonicalEvent event{};
        std::vector<unsigned char> payload;
        sze_recovery::RingReadStatus status=sze_recovery::kRingReadNotReady;
        std::uint64_t read_start=0, read_end=0;
    };
    ShmPrefetch(const sze_recovery::ShmEventRing& ring, int cpu, std::size_t capacity=4096)
        : ring_(ring), cpu_(cpu), capacity_(capacity), slots_(new Record[capacity]) {
        if (!capacity || cpu<0 || cpu>=CPU_SETSIZE || !ring_.is_open())
            throw std::runtime_error("cannot initialize SSE SHM prefetch");
        for (std::size_t i=0;i<capacity_;++i)
            slots_[i].payload.resize(ring_.header()->max_payload_bytes);
    }
    ~ShmPrefetch() { stop(); }
    ShmPrefetch(const ShmPrefetch&)=delete;
    ShmPrefetch& operator=(const ShmPrefetch&)=delete;

    sze_recovery::RingReadStatus read(std::uint64_t expected, const Record** result) {
        *result=nullptr;
        if (borrowed_) {
            consumed_.store(++read_index_,std::memory_order_release);
            borrowed_=false;
        }
        if (!thread_.joinable() || restart_ || expected!=next_event_) start(expected);
        if (published_.load(std::memory_order_acquire)==read_index_)
            return sze_recovery::kRingReadNotReady;
        const Record& record=slots_[read_index_%capacity_];
        if (record.status!=sze_recovery::kRingReadOk) {
            restart_=true;
            return record.status;
        }
        if (record.event.event_id!=expected)
            throw std::runtime_error("SSE prefetch event sequence mismatch");
        borrowed_=true;
        ++next_event_;
        *result=&record;
        return sze_recovery::kRingReadOk;
    }
    void stop() {
        stopping_.store(true,std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }
private:
    static void pause() { __builtin_ia32_pause(); }
    static std::uint64_t now_ns() {
        timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
        return std::uint64_t(ts.tv_sec)*1000000000ULL+ts.tv_nsec;
    }
    void start(std::uint64_t first) {
        stop();
        borrowed_=false;restart_=false;read_index_=0;next_event_=first;
        published_.store(0,std::memory_order_relaxed);
        consumed_.store(0,std::memory_order_relaxed);
        started_.store(false,std::memory_order_relaxed);
        startup_failed_=false;
        stopping_.store(false,std::memory_order_relaxed);
        thread_=std::thread([this,first] {
            cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(cpu_,&cpus);
            startup_failed_=pthread_setaffinity_np(pthread_self(),sizeof(cpus),&cpus)!=0;
            pthread_setname_np(pthread_self(),"sse-shm-reader");
            started_.store(true,std::memory_order_release);
            if (startup_failed_) return;
            std::uint64_t written=0;
            while (!stopping_.load(std::memory_order_acquire)) {
                if (written-consumed_.load(std::memory_order_acquire)>=capacity_) {pause();continue;}
                Record& record=slots_[written%capacity_];
#ifdef SSE_REPLAY_PROBE
                record.read_start=now_ns();
#endif
                record.status=ring_.read(first+written,&record.event,record.payload.data(),record.payload.size());
                if (record.status==sze_recovery::kRingReadNotReady) {pause();continue;}
#ifdef SSE_REPLAY_PROBE
                record.read_end=now_ns();
#endif
                const bool terminal=record.status!=sze_recovery::kRingReadOk;
                published_.store(++written,std::memory_order_release);
                if (terminal) return;
            }
        });
        while (!started_.load(std::memory_order_acquire)) pause();
        if (startup_failed_) {stop();throw std::runtime_error("SSE SHM reader CPU affinity failed");}
    }
    const sze_recovery::ShmEventRing& ring_;
    int cpu_;
    std::size_t capacity_;
    std::unique_ptr<Record[]> slots_;
    alignas(64) std::atomic<std::uint64_t> published_{0};
    alignas(64) std::atomic<std::uint64_t> consumed_{0};
    std::atomic<bool> stopping_{false}, started_{false};
    std::thread thread_;
    bool startup_failed_=false, borrowed_=false, restart_=false;
    std::uint64_t read_index_=0, next_event_=0;
};
} // namespace sse_journal
#endif
