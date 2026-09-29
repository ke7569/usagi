#ifndef SSE_T0_PARALLEL_PROCESSOR_H
#define SSE_T0_PARALLEL_PROCESSOR_H

#include "sse/runtime/sse_stream_processor.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <bitset>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <limits>
#include <set>
#include <sstream>
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>

#ifdef SSE_REPLAY_PROBE
extern std::atomic<bool> trace_active;
extern void trace_capture_factor(std::uint64_t*);
extern void trace_restore_factor(const std::uint64_t*);
extern void verify_output(const sse_stream::Output&);
extern void trace_wrapped_output(const sse_stream::Output&);
extern void trace_delivery(const sse_stream::Output&);
extern void trace_close(unsigned,std::uint64_t,std::uint64_t);
extern void trace_job(unsigned,std::uint64_t,int,std::uint64_t,std::uint64_t,std::uint64_t,std::size_t);
#endif
namespace sse_stream {

using OutputBatchCallback = std::function<void(const std::vector<Output>&)>;

// One decoder/batch coordinator, fixed stock owners, bounded SPSC job rings.
// Every record is published immediately. Each stock result is published only
// after its input BatchEnd and its preceding book updates. Other stocks do not
// hold that result; aggregate BatchEnd markers remain ordered for accounting.
// When SSE_STRATEGY_CPU is set, completed outputs cross one
// additional bounded SPSC ownership queue before the application callback.
class SseParallelProcessor {
public:
    static void* operator new(std::size_t size){void*p=0;if(posix_memalign(&p,64,size))throw std::bad_alloc();return p;}
    static void operator delete(void*p){std::free(p);}

    SseParallelProcessor(const sse_tick::DailyStaticMetadataMap& metadata,
        const sse_hybrid_model::Model* model, bool factors_only,
        const OutputBatchCallback& callback,
        const Auction59Provider& provider = Auction59Provider(),
        const sse_auction59::StaticMetadataMap& auction_metadata =
            sse_auction59::StaticMetadataMap(), bool auction_enabled = true)
        : callback_(callback), coordinator_(), workers_(), owner_by_number_(1000000,-1),
          valid_flags_(new std::atomic<unsigned char>[1000000]),
          touched_(), tickets_(), decided_(false), parallel_(false),
          failed_(false), stopping_(false), invalid_reason_(),
          timing_(std::getenv("SSE_OWNER_TIMING") != 0), completed_jobs_(0),
          submitted_jobs_(0), high_water_(0), queue_waits_(0),
          strategy_cpu_(configured_strategy_cpu()), async_delivery_(strategy_cpu_ >= 0),
          delivery_slots_(), delivery_thread_(), delivery_published_(0), delivery_consumed_(0),
          delivery_started_(false), delivery_stopping_(false), delivery_abort_(false),
          delivery_failed_(false), delivery_error_(), delivery_high_water_(0), delivery_full_waits_(0),
          strategy_poll_(), route_jobs_() {
        if (!callback_) throw std::runtime_error("SSE processor callback required");
        for (std::size_t i = 0; i < owner_by_number_.size(); ++i)
            valid_flags_[i].store(0, std::memory_order_relaxed);
        coordinator_.reset(new SseStreamProcessor(metadata, model, factors_only,
            [this](const Output& output) {
                callback_(std::vector<Output>(1, output));
            }, provider, auction_metadata, auction_enabled));
        std::vector<int> cpus;
        if (model && model->is_v06() && !factors_only) cpus = configured_cpus();
        if (cpus.size() < 2 || metadata.size() < 2) {
            async_delivery_ = false;
            decided_ = true;
            return;
        }
        cpus.resize(std::min(cpus.size(), metadata.size()));
        route_jobs_.resize(cpus.size(),nullptr);
        std::vector<sse_tick::DailyStaticMetadataMap> partitions(cpus.size());
        std::size_t next = 0;
        for (const auto& entry : metadata) {
            std::string code;
            if (!sse_tick::normalize_sse_security_id(entry.first, &code))
                throw std::runtime_error("invalid or duplicate SSE owner security");
            const auto number=security_number(code);
            if(number>=owner_by_number_.size()) throw std::runtime_error("invalid SSE owner code");
            if(owner_by_number_[number] != -1)
                throw std::runtime_error("invalid or duplicate SSE owner security");
            partitions[next].insert(entry);
            owner_by_number_[number]=static_cast<int>(next);
            valid_flags_[number].store(1, std::memory_order_release);
            next = (next + 1) % cpus.size();
        }
        try {
            for (std::size_t i = 0; i < cpus.size(); ++i) {
                std::unique_ptr<Worker> w(new Worker(cpus[i]));
                Worker* raw = w.get();
                raw->processor.reset(new SseStreamProcessor(partitions[i], model,
                    factors_only, [raw](Output&& output) {
#ifdef SSE_REPLAY_PROBE
                        std::array<std::uint64_t,2> t;trace_capture_factor(t.data());raw->current->factor_trace.push_back(t);
#endif

                        Job& job=*raw->current;
                        // Capacity was reserved before the first publication. The owner
                        // reads only released elements while this worker appends others.
                        job.outputs.push_back(std::move(output));
#ifdef SSE_CRITICAL_PATH_PROBE
                        job.output_ready_ns.push_back(now_ns());
#endif
                        {
                            job.factor_published.store(job.outputs.size(),std::memory_order_release);
                            if(!job.model_queued){
                                job.model_queued=true;
                                raw->model_slots[raw->model_written%kCapacity]=raw->book_sequence;
                                raw->model_published.store(++raw->model_written,std::memory_order_release);
                            }
                        }
                    }, provider, auction_metadata, auction_enabled));
                raw->processor->defer_model_=true;
                raw->processor->delegated_sequence_ = true;
                raw->processor->hardware_batch_mode_ = true;
                workers_.push_back(std::move(w));
            }
            for (auto& w : workers_){
                w->model_thread=std::thread(&SseParallelProcessor::model_main,this,w.get());
                w->thread = std::thread(&SseParallelProcessor::worker_main, this, w.get());
            }
            for (auto& w : workers_) {
                while (!w->started.load(std::memory_order_acquire)) pause();
                if (w->error) std::rethrow_exception(w->error);
            }
            if (async_delivery_) {
                delivery_slots_.reset(new DeliveryItem[kDeliveryCapacity]);
                delivery_thread_ = std::thread(&SseParallelProcessor::delivery_main, this);
                while (!delivery_started_.load(std::memory_order_acquire)) pause();
                if (delivery_failed_.load(std::memory_order_acquire))
                    check();
            }
            std::fprintf(stderr, "sse_owner_dispatch workers=%zu slots_per_worker=%zu publication=immediate batch_fence=input_closed result_delivery=per_stock\n",
                         workers_.size(), kCapacity);
            for (std::size_t i = 0; i < workers_.size(); ++i)
                std::fprintf(stderr, "sse_prediction_worker index=%zu cpu=%d tid=%ld instruments=%zu\n",
                    i, workers_[i]->cpu, workers_[i]->tid, partitions[i].size());
        } catch (...) {
            delivery_abort_.store(true, std::memory_order_release);
            stop_workers();
            stop_delivery();
            throw;
        }
    }

    ~SseParallelProcessor() {
        stop_workers();
        stop_delivery();
        if (timing_) {
            std::fprintf(stderr, "sse_owner_metrics jobs=%llu completed=%llu pending_high_water=%zu queue_full_waits=%llu\n",
                (unsigned long long)submitted_jobs_, (unsigned long long)completed_jobs_,
                high_water_, (unsigned long long)queue_waits_);
            dispatch_latency_.report("queue_to_worker");
            compute_latency_.report("worker_compute");
            close_latency_.report("batch_close_to_delivery");
        }
    }
    SseParallelProcessor(const SseParallelProcessor&) = delete;
    SseParallelProcessor& operator=(const SseParallelProcessor&) = delete;

    void on_event(const deepwin_market_data::StreamEvent& event) {
        check();
        try {
            if (!decided_ && event.kind == deepwin_market_data::kDatagramEvent) {
                decided_ = true;
                parallel_ = event.hardware_ns != 0 &&
                    (event.timestamp_flags & deepwin_market_data::kHardwareTimestampRequested);
                if (parallel_) {
                    coordinator_->route_raw_tick_ = [this](const sse_live::RawTickEvent& t,
                        const deepwin_market_data::StreamEvent& e, std::size_t o, std::uint64_t batch) { route_tick(t,e,o,batch); };
                    coordinator_->route_packet_ = [this](const deepwin_market_data::StreamEvent& e, std::uint64_t batch){route_packet(e,batch);};
                    coordinator_->route_raw_snapshot_ = [this](const unsigned char* p,const deepwin_market_data::StreamEvent&e,std::size_t off){route_raw_snapshot(p,e,off);};
                    coordinator_->route_snapshot_ = [this](const sse_live::Snapshot& s,
                        const deepwin_market_data::StreamEvent& e, std::size_t o) { route_snapshot(s,e,o); };
                    coordinator_->route_close_ = [this](std::uint32_t c,
                        const sse_live_sampling::BatchEnd& b) { route_close(c,b); };
                } else {
                    stop_workers();
                    if (async_delivery_) {
                        stop_strategy();
                        async_delivery_ = false;
                    }
                }
            }
#ifdef SSE_APP_LATENCY_PROBE
            const auto phase_start=now_ns();
#endif
            coordinator_->on_event(event);
#ifdef SSE_APP_LATENCY_PROBE
            const auto phase_middle=now_ns();
            sse_probe_add(1,phase_middle-phase_start);
#endif
            if (parallel_) drain_ready();
#ifdef SSE_APP_LATENCY_PROBE
            sse_probe_add(2,now_ns()-phase_middle);
#endif

        } catch (const std::exception& e) { fail(e.what()); }
        catch (...) { fail("unknown SSE owner dispatcher exception"); }
    }

    void begin_transport_epoch(std::uint64_t last_monotonic) {
        if(!coordinator_->hardware_batch_mode_)throw std::runtime_error("cross-boot replay requires hardware batch mode");
        deepwin_market_data::StreamEvent idle={};idle.kind=deepwin_market_data::kIdleEvent;idle.monotonic_ns=last_monotonic;
        coordinator_->on_event(idle);flush();
        coordinator_->hardware_batches_.clear();
        for(auto& worker:workers_)worker->processor->hardware_batches_.clear();
        // Preserve books, sample gates, model states and exchange channel
        // sequence checks. Only the host/PHC epoch boundary is reset.
    }
    void flush() {
        check();
        try {
            while (pending_events() != 0 || !tickets_.empty()) {
                drain_ready();
                for(auto& w:workers_)retire_ticks(*w);
                if (pending_events() != 0 || !tickets_.empty()) pause();
            }
        } catch (const std::exception& e) { fail(e.what()); }
        catch (...) { fail("unknown SSE owner drain exception"); }
    }

    bool invalid() const {
        return failed_.load(std::memory_order_acquire) || coordinator_->invalid();
    }
    const std::string& invalid_reason() const {
        return invalid_reason_.empty() ? coordinator_->invalid_reason() : invalid_reason_;
    }
    bool parallel_enabled() const { return !workers_.empty() && (!decided_ || parallel_); }
    std::size_t worker_count() const { return parallel_enabled() ? workers_.size() : 1; }
    std::size_t pending_events() const {
        std::size_t count=0;
        for(const auto& w:workers_) count+=w->written-w->retired;
        count += pending_delivery();
        return count;
    }
    std::size_t pending_delivery() const {
        return async_delivery_
            ? static_cast<std::size_t>(delivery_published_.load(std::memory_order_acquire) -
                                       delivery_consumed_.load(std::memory_order_acquire))
            : 0;
    }
    bool strategy_consumer_enabled() const { return async_delivery_; }
    int strategy_consumer_cpu() const { return strategy_cpu_; }
    bool strategy_consumer_valid() const {
        return !delivery_failed_.load(std::memory_order_acquire);
    }
    std::uint64_t strategy_consumer_published() const {
        return delivery_published_.load(std::memory_order_acquire);
    }
    std::uint64_t strategy_consumer_consumed() const {
        return delivery_consumed_.load(std::memory_order_acquire);
    }
    std::uint64_t strategy_consumer_high_water() const {
        return delivery_high_water_.load(std::memory_order_acquire);
    }
    std::uint64_t strategy_consumer_full_waits() const {
        return delivery_full_waits_.load(std::memory_order_acquire);
    }
    bool strategy_consumer_started() const {
        return !async_delivery_ || delivery_started_.load(std::memory_order_acquire);
    }
    void post_strategy(std::function<void()> task) {
        if (!task) throw std::invalid_argument("SSE strategy control task is empty");
        if (!async_delivery_) {
            task();
            return;
        }
        if (delivery_stopping_.load(std::memory_order_acquire))
            throw std::runtime_error("SSE strategy consumer is stopping");
        enqueue_control(std::move(task));
    }
    void wait_strategy() {
        if (!async_delivery_) return;
        const std::uint64_t target =
            delivery_published_.load(std::memory_order_acquire);
        while (delivery_consumed_.load(std::memory_order_acquire) < target) {
            check();
            pause();
        }
        check();
    }
    // Register only after application state is ready. The callback belongs to
    // the existing consumer and never runs on the constructing/producer thread.
    void set_strategy_poll(const std::function<void()>& poll) {
        post_strategy([this, poll]() { strategy_poll_ = poll; });
        wait_strategy();
    }
    void stop_strategy() {
        if (!async_delivery_) return;
        delivery_stopping_.store(true, std::memory_order_release);
        std::exception_ptr pending;
        try {
            if (!delivery_failed_.load(std::memory_order_acquire))
                wait_strategy();
        } catch (...) {
            pending = std::current_exception();
            delivery_abort_.store(true, std::memory_order_release);
        }
        if (delivery_thread_.joinable()) delivery_thread_.join();
        if (pending) std::rethrow_exception(pending);
        check();
    }
    // Diagnostic boundary: called by the owner thread after draining warmup.
    void reset_timings() {
        flush();
        dispatch_latency_=Histogram();compute_latency_=Histogram();close_latency_=Histogram();
        timing_=true;
    }
    void poll_completed() {
        try { drain_ready(); }
        catch(const std::exception& e) { fail(e.what()); }
    }
    bool instrument_static_valid(const std::string& code) const {
        if (failed_.load(std::memory_order_acquire) ||
            delivery_failed_.load(std::memory_order_acquire)) return false;
        if (!parallel_) return coordinator_->instrument_static_valid(code);
        const auto number = security_number(code);
        return number < owner_by_number_.size() &&
            valid_flags_[number].load(std::memory_order_acquire) != 0;
    }

private:
    static const std::size_t kCapacity = 16;
    static const std::size_t kDeliveryCapacity = 4096;
    enum Kind { Tick, Snapshot, Close, Packet };
    struct PacketRecord {unsigned char bytes[72];std::size_t offset;};
    struct DeliveryItem {
        enum ItemKind { OutputItem, ControlItem };
        ItemKind kind;
        std::vector<Output> outputs;
        std::function<void()> control;
        DeliveryItem() : kind(OutputItem), outputs(), control() {}
    };
    struct Histogram {
        std::array<std::uint64_t, 10002> bins;
        std::uint64_t count, sum, maximum;
        Histogram() : bins(), count(0), sum(0), maximum(0) {}
        void add(std::uint64_t ns) {
            ++count; sum += ns; maximum = std::max(maximum, ns);
            ++bins[std::min<std::uint64_t>(10001, ns / 1000)];
        }
        std::size_t percentile(unsigned p) const {
            if(!count) return 0;
            std::uint64_t n = 0, target = (count * p + 99) / 100;
            for (std::size_t i=0;i<bins.size();++i) {
                n += bins[i];
                if(n >= target) return i+1==bins.size() ? (maximum+999)/1000 : i+1;
            }
            return bins.size();
        }
        void report(const char* name) const {
            std::fprintf(stderr,"sse_owner_timing name=%s count=%llu mean_us=%.3f p50_upper_us=%zu p95_upper_us=%zu p99_upper_us=%zu max_us=%.3f\n",
                name,(unsigned long long)count,count?double(sum)/count/1000:0,
                percentile(50),percentile(95),percentile(99),double(maximum)/1000);
        }
    };
    struct Ticket;
    struct Job {
        Kind kind;
        deepwin_market_data::StreamEvent event;
        std::size_t offset;
        sse_live::RawTickEvent tick;
        std::uint64_t tick_batch;
        std::vector<PacketRecord> packet;
        std::unique_ptr<sse_live::Snapshot> snapshot;
        std::unique_ptr<unsigned char[]> snapshot_bytes;
        bool raw_snapshot=false;
        bool model_queued=false;
        sse_live_sampling::BatchEnd batch;
        std::vector<Output> outputs;
#ifdef SSE_REPLAY_PROBE
        std::vector<std::array<std::uint64_t,2>> factor_trace;
#endif

        std::atomic<std::size_t> output_published;
        std::atomic<std::size_t> factor_published{0};
        std::size_t output_consumed;
        Ticket* close_ticket;
        bool close_marker_seen;

#ifdef SSE_CRITICAL_PATH_PROBE
        std::vector<std::uint64_t> output_ready_ns;
#endif
        bool static_valid;
        std::uint64_t queued_ns, started_ns, done_ns;
        Job() : kind(Tick), event(), offset(0), tick(), tick_batch(0), snapshot(), batch(),
                outputs(), output_published(0), output_consumed(0), close_ticket(0), close_marker_seen(false), static_valid(true), queued_ns(0), started_ns(0), done_ns(0) {}
    };
#ifdef SSE_REPLAY_PROBE
    static bool replay_trace(const Job& j){return j.kind==Close&&trace_active.load(std::memory_order_acquire);}
#else
    static bool replay_trace(const Job&){return false;}
#endif
    struct Worker {
        // C++11/GCC 4.8 operator new does not guarantee over-alignment.
        static void* operator new(std::size_t size) {
            void* p=0; if(posix_memalign(&p,64,size)) throw std::bad_alloc(); return p;
        }
        static void operator delete(void* p) { std::free(p); }
        int cpu; long tid;
        std::unique_ptr<SseStreamProcessor> processor;
        std::unique_ptr<Job[]> slots;
        std::thread thread,model_thread;
        std::array<std::uint64_t,kCapacity> model_slots;
        std::uint64_t model_written=0,book_sequence=0;
        alignas(64) std::atomic<std::uint64_t> model_published{0};
        alignas(64) std::atomic<std::uint64_t> book_completed{0};
        std::atomic<bool> model_failed{false};std::exception_ptr model_error;
        alignas(64) std::atomic<std::uint64_t> published;
        alignas(64) std::atomic<std::uint64_t> completed;
        std::atomic<bool> started;
        std::atomic<bool> failed;
        alignas(64) std::uint64_t written, retired;
        std::size_t pending_results; // Decoder owner only; skip inactive worker cache lines.
        alignas(64) Job* current;
        std::exception_ptr error;
        Worker(int c) : cpu(c),tid(0),processor(),slots(new Job[kCapacity]),
            thread(),published(0),completed(0),started(false),failed(false),written(0),retired(0),pending_results(0),current(0),error() {}
    };
    struct Ticket {
        sse_live_sampling::BatchEnd batch;
        std::uint64_t closed_ns;
        std::size_t remaining;
        std::uint32_t candidates, predictions;
        Ticket() : batch(),closed_ns(0),remaining(0),candidates(0),predictions(0) {}
    };


    static std::uint64_t now_ns() {
        timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
        return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;
    }
    static void pause() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }
    static std::vector<int> configured_cpus() {
        std::vector<int> cpus;
        const char* value=std::getenv("SSE_PREDICTION_CPUS");
        if (!value || !*value) return cpus;
        std::string text(value); std::replace(text.begin(),text.end(),',',' ');
        std::istringstream in(text); std::set<int> unique; std::string token;
        while(in>>token) {
            char* end=0; errno=0; const long cpu=std::strtol(token.c_str(),&end,10);
            if(errno || !end || *end || cpu<0 || cpu>=CPU_SETSIZE || !unique.insert(cpu).second)
                throw std::runtime_error("invalid SSE_PREDICTION_CPUS: "+token);
            cpus.push_back(cpu);
        }
        if(cpus.empty()) throw std::runtime_error("empty SSE_PREDICTION_CPUS");
        return cpus;
    }
    static int configured_strategy_cpu() {
        const char* value = std::getenv("SSE_STRATEGY_CPU");
        if (!value || !*value) return -1;
        char* end = 0;
        errno = 0;
        const long cpu = std::strtol(value, &end, 10);
        if (errno || end == value || !end || *end || cpu < 0 || cpu >= CPU_SETSIZE)
            throw std::runtime_error(std::string("invalid SSE_STRATEGY_CPU: ") + value);
        return static_cast<int>(cpu);
    }
    static std::size_t security_number(const std::string& code) {
        if(code.size()!=6) return 1000000;
        std::size_t n=0;
        for(char c:code) { if(c<'0' || c>'9') return 1000000; n=n*10+(c-'0'); }
        return n;
    }
    int owner_for(const std::string& code) const {
        const auto n=security_number(code);
        return n<owner_by_number_.size()?owner_by_number_[n]:-1;
    }

    Job& reserve(std::size_t index, Kind kind) {
        Worker& w=*workers_[index];
        // Reclaim already-consumed plain ticks only when this owner's ring
        // needs space; publishing and processing still happen per record.
        if(w.written-w.retired >= kCapacity/2)retire_ticks(w);
        if(w.written-w.retired == kCapacity) ++queue_waits_;
        while(w.written-w.retired == kCapacity) {
#ifdef SSE_OWNER_TEST_PROBE
            sse_owner_test_full();
#endif
            drain_ready();retire_ticks(w);check();pause();
        }
        Job& job=w.slots[w.written % kCapacity];
        // Ordinary book updates never read or produce result storage. Avoid
        // pulling snapshot/close/result cache lines across cores on every tick.
        if(kind!=Tick && kind!=Packet) {
        job.outputs.clear(); job.batch.candidates.clear();job.model_queued=false;
#ifdef SSE_REPLAY_PROBE
        job.factor_trace.clear();
#endif

        job.output_published.store(0,std::memory_order_relaxed);job.factor_published.store(0,std::memory_order_relaxed);
        job.output_consumed=0;job.close_ticket=0;job.close_marker_seen=false;
#ifdef SSE_CRITICAL_PATH_PROBE
        job.output_ready_ns.clear();
#endif
        }
        return job;
    }
    void publish(std::size_t index) {
        Worker& w=*workers_[index];
        Job& job=w.slots[w.written % kCapacity];
        if(timing_ || replay_trace(job))job.queued_ns=now_ns();
        const auto sequence=++w.written;
        w.published.store(sequence,std::memory_order_release);
        ++submitted_jobs_;

    }
    void append(Ticket&& ticket) {
        tickets_.push_back(std::move(ticket));
        high_water_=std::max(high_water_,tickets_.size());
    }
    std::uint64_t reserve_delivery(std::size_t weight) {
        std::uint64_t write = delivery_published_.load(std::memory_order_relaxed);
        for (;;) {
            const auto pending = write - delivery_consumed_.load(std::memory_order_acquire);
            // Keep the former Output/control budget when one slot owns a batch.
            // An oversized indivisible batch may proceed only into an empty queue.
            const bool fits = weight > kDeliveryCapacity ? pending == 0
                : pending <= kDeliveryCapacity - weight;
            if (fits) break;
            delivery_full_waits_.fetch_add(1, std::memory_order_relaxed);
            check();
            pause();
        }
        return write;
    }
    void publish_delivery(std::uint64_t write, std::size_t weight) {
        // A batch occupies consecutive logical Output credits, stored only at
        // their first slot. The consumer advances by the same batch length.
        delivery_published_.store(write + weight, std::memory_order_release);
        const auto pending = write + weight - delivery_consumed_.load(std::memory_order_acquire);
        std::uint64_t high = delivery_high_water_.load(std::memory_order_relaxed);
        while (pending > high &&
               !delivery_high_water_.compare_exchange_weak(
                   high, pending, std::memory_order_release, std::memory_order_relaxed)) {}
    }
    void deliver(std::vector<Output>&& outputs) {
#ifdef SSE_REPLAY_PROBE
        if(trace_active.load(std::memory_order_acquire))for(const auto&out:outputs)trace_delivery(out);
#endif

        if (!async_delivery_) {
            callback_(outputs);
            return;
        }
        if (delivery_stopping_.load(std::memory_order_acquire))
            throw std::runtime_error("SSE strategy consumer is stopping");
        const std::size_t weight = std::max<std::size_t>(1, outputs.size());
        const std::uint64_t write = reserve_delivery(weight);
        DeliveryItem& item = delivery_slots_[write % kDeliveryCapacity];
        item.kind = DeliveryItem::OutputItem;
        item.control = std::function<void()>();
        item.outputs = std::move(outputs);
        publish_delivery(write, weight);
    }
    void enqueue_control(std::function<void()> task) {
        if (delivery_stopping_.load(std::memory_order_acquire))
            throw std::runtime_error("SSE strategy consumer is stopping");
        const std::uint64_t write = reserve_delivery(1);
        DeliveryItem& item = delivery_slots_[write % kDeliveryCapacity];
        item.kind = DeliveryItem::ControlItem;
        item.control = std::move(task);
        publish_delivery(write, 1);
    }
    void route_tick(const sse_live::RawTickEvent& tick,
                    const deepwin_market_data::StreamEvent& event, std::size_t offset, std::uint64_t batch) {
        const int owner=tick.security_number<owner_by_number_.size()?owner_by_number_[tick.security_number]:-1;
        if(owner<0) return;
        Job& j=reserve(owner,Tick);
        j.kind=Tick; j.tick=tick; j.event=event; j.event.data=0; j.offset=offset;
        j.tick_batch=batch;
        touched_[event.channel_id].set(owner);
        // A book update has no output. Its FIFO successor (snapshot/close)
        // already establishes the required dependency; no global ticket is
        // needed for every tick record.
        publish(owner);
    }
    void route_packet(const deepwin_market_data::StreamEvent& event,std::uint64_t batch) {
        auto& jobs=route_jobs_;std::fill(jobs.begin(),jobs.end(),nullptr);
        for(std::size_t off=0;off<event.size;off+=72) {
            const auto* bytes=event.data+off;
            std::uint64_t wire;std::uint32_t provider;
            std::memcpy(&wire,bytes+9,8);std::memcpy(&provider,bytes,4);
            const unsigned channel=bytes[17]|(unsigned(bytes[18])<<8);
            unsigned security=0;for(unsigned n=0;n<6;++n){const unsigned d=bytes[21+n]-'0';if(d>9)throw std::runtime_error("invalid SSE packet security");security=security*10+d;}
            if(!channel||!wire)throw std::runtime_error("invalid SSE packet sequence");
            const unsigned char type=bytes[34];std::uint32_t raw_time;std::memcpy(&raw_time,bytes+30,4);raw_time/=100;
            if((type!='A'&&type!='D'&&type!='T'&&type!='S')||raw_time%100>59||(raw_time/100)%100>59||raw_time/10000>23)throw std::runtime_error("invalid tick type/time");
            // Decode validation moves to each stock owner; the input owner
            // retains authoritative full-channel sequence checks.
            const int owner=security<owner_by_number_.size()?owner_by_number_[security]:-1;
            if(owner<0){sse_live::RawTickEvent raw;if(!sse_live::decode_primary_raw_tick(bytes,72,&raw,0,false))throw std::runtime_error("invalid unconfigured tick");}
            if(!coordinator_->valid_tick_sequence(channel,wire,provider,event,off))continue;
            if(owner<0)continue;
            if(!jobs[owner]) {
                Job& j=reserve(owner,Packet);j.kind=Packet;j.event=event;j.event.data=0;j.tick_batch=batch;
                j.packet.clear();jobs[owner]=&j;touched_[event.channel_id].set(owner);
            }
            PacketRecord r;r.offset=off;std::memcpy(r.bytes,bytes,72);jobs[owner]->packet.push_back(r);
        }
        for(std::size_t owner=0;owner<jobs.size();++owner)if(jobs[owner])publish(owner);
    }
    void route_raw_snapshot(const unsigned char* bytes,const deepwin_market_data::StreamEvent& event,std::size_t offset) {
        std::uint32_t seq;std::memcpy(&seq,bytes+21,4);if(!seq)throw std::runtime_error("invalid snapshot sequence");
        unsigned security=0;for(unsigned n=0;n<6;++n){unsigned d=bytes[30+n]-'0';if(d>9)throw std::runtime_error("invalid snapshot security");security=security*10+d;}
        int owner=security<owner_by_number_.size()?owner_by_number_[security]:-1;
        if(owner<0){sse_live::Snapshot s;if(!sse_live::decode_primary_snapshot(bytes,440,&s,0,false))throw std::runtime_error("invalid unconfigured snapshot");return;}
        Job& j=reserve(owner,Snapshot);j.kind=Snapshot;j.raw_snapshot=true;
        if(!j.snapshot)j.snapshot.reset(new sse_live::Snapshot);
        if(!j.snapshot_bytes)j.snapshot_bytes.reset(new unsigned char[440]);
        std::memcpy(j.snapshot_bytes.get(),bytes,440);j.event=event;j.event.data=0;j.offset=offset;
        ++workers_[owner]->pending_results;publish(owner);
    }
    void route_snapshot(const sse_live::Snapshot& snapshot,
                        const deepwin_market_data::StreamEvent& event, std::size_t offset) {
        const int owner=owner_for(snapshot.security_id);
        if(owner<0) return;
        Job& j=reserve(owner,Snapshot);
        j.kind=Snapshot; j.raw_snapshot=false; if(!j.snapshot)j.snapshot.reset(new sse_live::Snapshot); *j.snapshot=snapshot; j.event=event; j.event.data=0; j.offset=offset;
        ++workers_[owner]->pending_results;
        publish(owner);
    }
    void route_close(std::uint32_t channel, const sse_live_sampling::BatchEnd& batch) {
        Ticket ticket;ticket.batch=batch;ticket.closed_ns=timing_?now_ns():0;
        auto& touched=touched_[channel];
#ifdef SSE_REPLAY_PROBE
        if(trace_active.load(std::memory_order_acquire))trace_close(channel,batch.batch_id,now_ns());
#endif

        ticket.remaining=touched.count();
        append(std::move(ticket));
        Ticket* pending=&tickets_.back();
        for(std::size_t index=0;index<workers_.size();++index) {
            if(!touched.test(index)) continue;
            Job& j=reserve(index,Close);j.kind=Close;j.event.channel_id=channel;j.batch=batch;
            j.close_ticket=pending;
            ++workers_[index]->pending_results;
            publish(index);
        }
        touched.reset();
    }

    void delivery_main() noexcept {
        try {
            pthread_setname_np(pthread_self(), "sse-strategy");
            cpu_set_t set; CPU_ZERO(&set); CPU_SET(strategy_cpu_, &set);
            if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set))
                throw std::runtime_error("SSE strategy consumer affinity failed");
            cpu_set_t actual; CPU_ZERO(&actual);
            if (pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual) ||
                CPU_COUNT(&actual) != 1 || !CPU_ISSET(strategy_cpu_, &actual))
                throw std::runtime_error("SSE strategy consumer affinity verification failed");
            delivery_started_.store(true, std::memory_order_release);
            std::uint64_t next = 0;
            for (;;) {
                if (delivery_abort_.load(std::memory_order_acquire)) return;
                const std::uint64_t published =
                    delivery_published_.load(std::memory_order_acquire);
                if (next == published) {
                    if (delivery_stopping_.load(std::memory_order_acquire)) return;
                    if (strategy_poll_) strategy_poll_();
                    pause();
                    continue;
                }
                DeliveryItem& item = delivery_slots_[next % kDeliveryCapacity];
                const std::size_t weight = item.kind == DeliveryItem::ControlItem ? 1
                    : std::max<std::size_t>(1, item.outputs.size());
                if (item.kind == DeliveryItem::ControlItem) {
                    item.control();
                } else {
                    // Release a completed batch after its callback, rather than
                    // retain up to one full batch in every reusable queue slot.
                    std::vector<Output> outputs=std::move(item.outputs);
                    if (strategy_poll_) strategy_poll_();
                    callback_(outputs);
                }
                next += weight;
                delivery_consumed_.store(next, std::memory_order_release);
            }
        } catch (...) {
            delivery_error_ = std::current_exception();
            delivery_failed_.store(true, std::memory_order_release);
            delivery_abort_.store(true, std::memory_order_release);
            delivery_stopping_.store(true, std::memory_order_release);
            delivery_started_.store(true, std::memory_order_release);
        }
    }

    void model_main(Worker* w) noexcept {
        try {
            cpu_set_t set;CPU_ZERO(&set);CPU_SET(w->cpu+1,&set);
            if(pthread_setaffinity_np(pthread_self(),sizeof(set),&set))throw std::runtime_error("model CPU affinity failed");
            std::uint64_t model_next=0;std::size_t n=0;
            while(!stopping_.load(std::memory_order_acquire)){
                if(w->model_published.load(std::memory_order_acquire)==model_next){pause();continue;}
                const auto next=w->model_slots[model_next%kCapacity];
                Job& j=w->slots[next%kCapacity];
                if(j.kind==Close||j.kind==Snapshot){
                    const auto ready=j.factor_published.load(std::memory_order_acquire);
                    for(;n<ready;++n){
                        auto& out=j.outputs[n];
                        if(out.kind==kTickOutput){
#ifdef SSE_REPLAY_PROBE
                            trace_restore_factor(j.factor_trace[n].data());
#endif

                            const bool selected=out.tick.prediction.selected;
                            auto*state=w->processor->state_for(out.tick.event.security_id);std::string error;
                            if(!state || !coordinator_->model_->on_tick(out.tick.factors.values,"sse",out.tick.event.time_of_day_micros,&state->model_state,&out.tick.prediction,&error))throw std::runtime_error("async model: "+error);
                            if(!selected){out.tick.prediction.selected=false;out.tick.prediction.selected_source=sse_hybrid_model::kNoSource;out.tick.prediction.selected_pred=0;}
                        }

#ifdef SSE_REPLAY_PROBE
                        verify_output(out);if(trace_active.load(std::memory_order_acquire))trace_wrapped_output(out);
#endif
                        j.output_published.store(n+1,std::memory_order_release);
#ifdef SSE_OWNER_TEST_PROBE
                        sse_owner_test_output(out);
#endif
                    }
                }
                if(w->book_completed.load(std::memory_order_acquire)<=next){pause();continue;}
                if((j.kind==Close||j.kind==Snapshot)&&n<j.factor_published.load(std::memory_order_acquire))continue;
#ifdef SSE_REPLAY_PROBE
                if(j.kind==Close && trace_active.load(std::memory_order_acquire))trace_job(j.event.channel_id,j.batch.batch_id,w->cpu,j.queued_ns,j.started_ns,now_ns(),j.outputs.size()-1);
#endif
                w->completed.store(next+1,std::memory_order_release);++model_next;n=0;
            }
        }catch(...){w->model_error=std::current_exception();w->model_failed.store(true,std::memory_order_release);failed_.store(true,std::memory_order_release);}
    }
    void worker_main(Worker* w) noexcept {
        try {
            w->tid=static_cast<long>(::syscall(SYS_gettid));
            cpu_set_t set; CPU_ZERO(&set); CPU_SET(w->cpu,&set);
            if(pthread_setaffinity_np(pthread_self(),sizeof(set),&set))
                throw std::runtime_error("SSE worker affinity failed");
            cpu_set_t actual; CPU_ZERO(&actual);
            if(pthread_getaffinity_np(pthread_self(),sizeof(actual),&actual) ||
               CPU_COUNT(&actual)!=1 || !CPU_ISSET(w->cpu,&actual))
                throw std::runtime_error("SSE worker affinity verification failed");
            w->started.store(true,std::memory_order_release);
            std::uint64_t next=0;
            sse_live::TickEvent tick;bool prepare=false;
            while(!stopping_.load(std::memory_order_acquire)) {
                if(w->published.load(std::memory_order_acquire)==next) {
                    if(prepare){prepare=false;auto*state=w->processor->state_for(tick.security_id);
                        if(state && state->have_pending_tick && state->sample_gate.initialized()){
                            auto gate=state->sample_gate;sse_live_sampling::SampleDecision decision;std::string error;
                            const auto cut=w->processor->book_cut(*state,state->pending_tick);
                            if(gate.observe_batch_end(cut,state->factors.turnover_threshold(),&decision,&error)&&decision.accepted)state->factors.prepare_book(state->book,state->pending_tick.time_of_day_micros);
                        }}pause();continue;
                }
                prepare=false;
                Job& j=w->slots[next % kCapacity];w->current=&j;w->book_sequence=next;
                if(timing_ || replay_trace(j)) j.started_ns=now_ns();
                auto& p=*w->processor;
                if(j.kind==Packet) {
                    auto& batch=p.hardware_batches_[j.event.channel_id];
                    if(!batch.open){batch.open=true;batch.batch_id=j.tick_batch;batch.candidates.clear();}
                    if(batch.batch_id!=j.tick_batch)throw std::runtime_error("SSE packet missing batch close");
                    for(const auto& r:j.packet){
                        sse_live::RawTickEvent raw;
                        if(!sse_live::decode_primary_raw_tick(r.bytes,72,&raw,0,false))throw std::runtime_error("invalid owner packet tick");
#ifdef SSE_OWNER_TEST_PROBE
                        sse_owner_test_tick(raw);
#endif
                        sse_live::materialize_tick(raw,&tick);p.process_tick(tick,j.event,r.offset);
                    }

                } else if(j.kind==Tick) {
#ifdef SSE_OWNER_TEST_PROBE
                    sse_owner_test_tick(j.tick);
#endif
                    auto& b=p.hardware_batches_[j.event.channel_id];
                    if(!b.open) { b.open=true; b.batch_id=j.tick_batch; b.candidates.clear(); }
                    if(b.batch_id!=j.tick_batch) throw std::runtime_error("SSE owner missing batch close");
                    sse_live::materialize_tick(j.tick,&tick);
                    p.process_tick(tick,j.event,j.offset);
                } else if(j.kind==Snapshot) {
                    j.outputs.reserve(1);
#ifdef SSE_REPLAY_PROBE
                    j.factor_trace.reserve(1);
#endif

#ifdef SSE_CRITICAL_PATH_PROBE
                    j.output_ready_ns.reserve(1);
#endif
                    if(j.raw_snapshot && !sse_live::decode_primary_snapshot(j.snapshot_bytes.get(),440,j.snapshot.get(),0,false))throw std::runtime_error("invalid owner snapshot");
                    p.process_snapshot(*j.snapshot,j.event,j.offset);
                    j.static_valid=p.instrument_static_valid(j.snapshot->security_id);
                } else {
                    auto& b=p.hardware_batches_.at(j.event.channel_id);
                    if(!b.open || b.batch_id!=j.batch.batch_id)
                        throw std::runtime_error("SSE owner batch boundary mismatch");
                    for(const auto& c:b.candidates) j.batch.candidates.push_back(c.second);
                    b.candidates.clear(); b.open=false;
                    j.outputs.reserve(j.batch.candidates.size()+1);
#ifdef SSE_REPLAY_PROBE
                    j.factor_trace.reserve(j.batch.candidates.size()+1);
#endif

#ifdef SSE_CRITICAL_PATH_PROBE
                    j.output_ready_ns.reserve(j.batch.candidates.size()+1);
#endif
                    p.process_closed_batch(j.batch);
                }
                if(timing_ || replay_trace(j)) j.done_ns=now_ns();

                prepare=j.kind==Packet||j.kind==Tick;
                {
                    if((j.kind==Close||j.kind==Snapshot)&&!j.model_queued){
                        j.model_queued=true;w->model_slots[w->model_written%kCapacity]=next;
                        w->model_published.store(++w->model_written,std::memory_order_release);
                    }
                    w->book_completed.store(++next,std::memory_order_release);
                }
            }
        } catch (...) {
            w->error=std::current_exception();
            w->failed.store(true,std::memory_order_release);
            failed_.store(true,std::memory_order_release);
            w->started.store(true,std::memory_order_release);
        }
    }

    void drain_ready() {
        check();
        // No global head-of-line dependency: advance each stock owner's FIFO.
        for(std::size_t index=0;index<workers_.size();++index) {
            Worker& w=*workers_[index];
            if(!w.pending_results)continue;
            retire_ticks(w);
            if(w.retired==w.written)continue;
            Job& j=w.slots[w.retired%kCapacity];
            if(j.kind==Tick || j.kind==Packet)continue;
            const bool done=w.completed.load(std::memory_order_acquire)>w.retired;
            if(j.kind==Snapshot) {
                if(!done)continue;
                const auto number=security_number(j.snapshot->security_id);
                if(number<owner_by_number_.size())
                    valid_flags_[number].store(j.static_valid?1:0,std::memory_order_release);
                if(!j.outputs.empty())deliver(std::move(j.outputs));
            } else {
                const auto ready=j.output_published.load(std::memory_order_acquire);
                while(j.output_consumed<ready) {
                    const std::size_t n=j.output_consumed++;
                    Output& out=j.outputs[n];
                    if(out.kind==kBatchEndOutput) {
                        if(j.close_marker_seen || out.batch_end.batch_id!=j.batch.batch_id)
                            throw std::runtime_error("SSE owner invalid close result");
                        j.close_marker_seen=true;
                        j.close_ticket->candidates+=out.batch_end.candidate_count;
                        j.close_ticket->predictions+=out.batch_end.prediction_count;
                    } else {
#ifdef SSE_CRITICAL_PATH_PROBE
                        sse_critical_path_probe(out,j.close_ticket->closed_ns,j.queued_ns,j.started_ns,
                            j.output_ready_ns[n],done?j.done_ns:0,now_ns(),index,0);
#endif
                        std::vector<Output> one;
                        one.push_back(std::move(out));
                        deliver(std::move(one));
                    }
                }
                if(!done)continue;
                if(!j.close_marker_seen)throw std::runtime_error("SSE owner missing close result");
                --j.close_ticket->remaining;
            }
            if(timing_) {
                dispatch_latency_.add(j.started_ns-j.queued_ns);
                compute_latency_.add(j.done_ns-j.started_ns);
            }
            --w.pending_results;++w.retired;++completed_jobs_;
            retire_ticks(w);
        }
        // These markers describe total batch counts. Predictions have already
        // crossed the input fence and never wait for these accounting markers.
        while(!tickets_.empty() && !tickets_.front().remaining) {
            const Ticket& ticket=tickets_.front();
            Output marker;marker.kind=kBatchEndOutput;
            marker.batch_end.batch_id=ticket.batch.batch_id;
            marker.batch_end.last_hardware_ns=ticket.batch.last_activity_ns;
            marker.batch_end.emitted_monotonic_ns=ticket.batch.emitted_ns;
            marker.batch_end.packet_count=ticket.batch.packet_count;
            marker.batch_end.candidate_count=ticket.candidates;
            marker.batch_end.prediction_count=ticket.predictions;
            std::vector<Output> one;one.push_back(std::move(marker));deliver(std::move(one));
            if(timing_)close_latency_.add(now_ns()-ticket.closed_ns);
            tickets_.pop_front();
        }
    }
    void retire_ticks(Worker& w) {
        const auto completed=w.book_completed.load(std::memory_order_acquire);
        while(w.retired<completed) {
            const Job& j=w.slots[w.retired%kCapacity];
            if(j.kind!=Tick && j.kind!=Packet) break;
            if(timing_) {
                dispatch_latency_.add(j.started_ns-j.queued_ns);
                compute_latency_.add(j.done_ns-j.started_ns);
            }
            ++w.retired; ++completed_jobs_;
        }
    }
    void check() const {
        if (delivery_failed_.load(std::memory_order_acquire)) {
            if (delivery_error_) std::rethrow_exception(delivery_error_);
            throw std::runtime_error("SSE strategy consumer failed");
        }
        if(failed_.load(std::memory_order_acquire)) {
            for (const auto& w:workers_)
                if(w->failed.load(std::memory_order_acquire)) std::rethrow_exception(w->error);
            for (const auto& w:workers_)
                if(w->model_failed.load(std::memory_order_acquire)) std::rethrow_exception(w->model_error);
            throw std::runtime_error(invalid_reason_.empty()?"SSE owner worker failed":invalid_reason_);
        }
        if(coordinator_->invalid()) throw std::runtime_error(coordinator_->invalid_reason());
    }
    void fail(const std::string& why) {
        if(invalid_reason_.empty()) invalid_reason_=why;
        failed_.store(true,std::memory_order_release);
        delivery_abort_.store(true, std::memory_order_release);
        delivery_stopping_.store(true, std::memory_order_release);
        throw std::runtime_error(invalid_reason_);
    }
    void stop_workers() {
        stopping_.store(true,std::memory_order_release);
        for(auto& w:workers_) {if(w->thread.joinable())w->thread.join();if(w->model_thread.joinable())w->model_thread.join();}
    }
    void stop_delivery() {
        if (!delivery_thread_.joinable()) return;
        delivery_stopping_.store(true, std::memory_order_release);
        delivery_thread_.join();
    }

    OutputBatchCallback callback_;
    std::unique_ptr<SseStreamProcessor> coordinator_;
    std::vector<std::unique_ptr<Worker> > workers_;
    std::vector<int> owner_by_number_;
    std::unique_ptr<std::atomic<unsigned char>[]> valid_flags_;
    std::map<std::uint32_t,std::bitset<CPU_SETSIZE> > touched_;
    std::deque<Ticket> tickets_;
    bool decided_,parallel_;
    // Workers read these on every iteration. Keep producer counters and
    // mutable ticket bookkeeping off their cache lines.
    alignas(64) std::atomic<bool> failed_;
    alignas(64) std::atomic<bool> stopping_;
    alignas(64) std::string invalid_reason_;
    bool timing_;
    std::uint64_t completed_jobs_,submitted_jobs_;
    std::size_t high_water_;
    std::uint64_t queue_waits_;
    Histogram dispatch_latency_,compute_latency_,close_latency_;
    int strategy_cpu_;
    bool async_delivery_;
    std::unique_ptr<DeliveryItem[]> delivery_slots_;
    std::thread delivery_thread_;
    alignas(64) std::atomic<std::uint64_t> delivery_published_;
    alignas(64) std::atomic<std::uint64_t> delivery_consumed_;
    std::atomic<bool> delivery_started_;
    alignas(64) std::atomic<bool> delivery_stopping_;
    std::atomic<bool> delivery_abort_;
    std::atomic<bool> delivery_failed_;
    std::exception_ptr delivery_error_;
    std::atomic<std::uint64_t> delivery_high_water_;
    std::atomic<std::uint64_t> delivery_full_waits_;
    std::function<void()> strategy_poll_;
    std::vector<Job*> route_jobs_;
};
} // namespace sse_stream
#endif
