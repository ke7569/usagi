#include "sse_event_pipeline.h"
#include "sse/runtime/sse_cpu_affinity.h"
#include "sse_event_queue.h"
#include "sse/runtime/sse_shard_plan.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <immintrin.h>

namespace sse_pipeline {
namespace {
struct Producer {
    explicit Producer(std::size_t capacity) : queue(capacity), submitted(0), overflow(0) {}
    EventQueue queue;
    std::uint64_t submitted, overflow;
};

struct Worker {
    explicit Worker(std::size_t capacity)
        : queue(capacity), consumed(0), overflow(0), errors(0),
          service_latency(), end_to_end_latency(), sample_end_to_end_latency() {}
    EventQueue queue;
    EventConsumer callback;
    std::thread thread;
    std::uint64_t consumed, overflow, errors;
    LatencyHistogram service_latency;
    LatencyHistogram end_to_end_latency;
    LatencyHistogram sample_end_to_end_latency;
};

struct SequenceWindow {
    std::uint64_t highest = 0;
    std::deque<std::uint64_t> order;
    std::unordered_set<std::uint64_t> seen;
};

struct PendingEvent {
    Event event;
    std::uint64_t origin_ns;
    PendingEvent() : event(), origin_ns(0U) {}
    PendingEvent(const Event& value, std::uint64_t origin)
        : event(value), origin_ns(origin) {}
};

struct TimedTick {
    Event event;
    std::uint64_t origin_ns;
    std::uint64_t due_ns;
    TimedTick() : event(), origin_ns(0U), due_ns(0U) {}
    TimedTick(const Event& value, std::uint64_t origin, std::uint64_t due)
        : event(value), origin_ns(origin), due_ns(due) {}
};

std::uint64_t monotonic_now_ns() {
    // steady_clock is backed by the platform monotonic clock on the supported
    // Linux toolchains and avoids a librt dependency on older glibc systems.
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t add_ns_saturated(std::uint64_t value, std::uint64_t delta) {
    const std::uint64_t maximum =
        (std::numeric_limits<std::uint64_t>::max)();
    return delta > maximum - value ? maximum : value + delta;
}

std::uint64_t elapsed_ns(std::uint64_t now, std::uint64_t origin) {
    return origin != 0U && now >= origin ? now - origin : 0U;
}

bool valid_event(const Event& event) {
    if (event.version != kEventVersion || event.trading_day == 0 ||
        event.security_id[6] != 0 || event.security_id[7] != 0) return false;
    for (int i = 0; i < 6; ++i)
        if (event.security_id[i] < '0' || event.security_id[i] > '9') return false;
    return ((event.kind == kTick || event.kind == kTickSample) &&
            event.payload_size == 72 && event.channel_no >= 1 && event.channel_no <= 6) ||
           (event.kind == kSnapshot && event.payload_size == 440 && event.channel_no <= 6);
}
}

struct EventPipeline::Impl {
    PipelineOptions options;
    ShardPlan plan;
    std::vector<std::unique_ptr<Producer> > ingress;
    std::vector<std::unique_ptr<Worker> > workers;
    Worker journal;
    std::thread dispatcher;
    std::atomic<bool> accepting, dispatch_done, compute_healthy, journal_healthy;
    std::atomic<unsigned> ready;
    std::atomic<unsigned> barrier_count;
    bool started;
    mutable std::mutex error_mutex;
    std::string last_error;
    std::uint64_t events, duplicates, late_ticks, pending_count, pending_overflow, routing_errors;
    LatencyHistogram ingress_latency;
    SequenceWindow tick_sequences[6];
    std::unordered_map<std::string, std::uint64_t> snapshot_sequences;
    std::unordered_map<std::string, std::deque<PendingEvent> > pending;
    std::map<std::pair<std::uint64_t, std::string>, std::string> sampling_due;
    std::map<std::string, TimedTick> last_ticks;
    std::uint64_t scheduler_now_ns;

    explicit Impl(const PipelineOptions& value) : options(value), plan(value.shards),
        journal(value.journal_capacity), accepting(false), dispatch_done(false),
        compute_healthy(true), journal_healthy(true), ready(0), barrier_count(0), started(false), events(0), duplicates(0), late_ticks(0),
        pending_count(0), pending_overflow(0), routing_errors(0), ingress_latency(),
        scheduler_now_ns(0U) {
        if (!plan.valid() || value.producers == 0 || value.producers > 64 ||
            value.shards > 128 || value.pending_snapshot_capacity == 0)
            throw std::invalid_argument("invalid SSE pipeline dimensions");
        for (std::size_t i = 0; i < value.producers; ++i)
            ingress.emplace_back(new Producer(value.ingress_capacity));
        for (std::size_t i = 0; i < value.shards; ++i)
            workers.emplace_back(new Worker(value.shard_capacity));
    }

    void fail(const std::string& message) {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (last_error.empty()) last_error = message;
    }

    bool bind(int cpu) {
        std::string error;
        const bool ok = cpu < 0 || sse_cpu::bind_current_thread(cpu, &error);
        if (!ok) { fail(error); compute_healthy = false; journal_healthy = false; }
        ready.fetch_add(1);
        return ok;
    }

    void consume(Worker* worker, int cpu, bool disk) {
        if (!bind(cpu)) return;
        Event event;
        std::uint64_t origin_ns = 0U;
        while (!dispatch_done.load(std::memory_order_acquire) || worker->queue.size() != 0) {
            if (!worker->queue.pop(&event, &origin_ns)) {
                _mm_pause();
                continue;
            }
            if (event.kind==0 && event.trading_day==0) { ++barrier_count; continue; }
            const std::uint64_t service_start_ns = monotonic_now_ns();
            bool ok = false;
            try { ok = worker->callback(event); }
            catch (const std::exception& error) { fail(error.what()); }
            catch (...) { fail("SSE consumer threw an unknown exception"); }
            const std::uint64_t service_end_ns = monotonic_now_ns();
            if(event.receive_realtime_ns >= options.latency_start_realtime_ns) worker->service_latency.observe(
                elapsed_ns(service_end_ns, service_start_ns));
            if (origin_ns != 0U && event.receive_realtime_ns >= options.latency_start_realtime_ns) {
                const std::uint64_t end_to_end =
                    elapsed_ns(service_end_ns, origin_ns);
                worker->end_to_end_latency.observe(end_to_end);
                if (event.kind == kTickSample)
                    worker->sample_end_to_end_latency.observe(end_to_end);
            }
            if (ok) ++worker->consumed;
            else {
                ++worker->errors;
                if (disk) journal_healthy = false; else compute_healthy = false;
                fail(disk ? "SSE journal consumer failed" : "SSE shard consumer failed");
            }
        }
    }

    bool deliver(Event event, std::uint64_t origin_ns,
                 std::uint32_t channel, std::uint32_t shard) {
        event.channel_no = static_cast<std::uint16_t>(channel);
        event.shard_id = shard;
        while (!workers[shard]->queue.push(event, origin_ns)) {
            // Replay is an offline producer and must not lose persisted
            // events. Live capture keeps the original non-blocking contract.
            if (options.replay || event.receive_realtime_ns < options.history_warmup_until_ns) {
                std::this_thread::yield();
                continue;
            }
            ++workers[shard]->overflow;
            compute_healthy = false;
            fail("SSE shard queue overflow; capture/journal remain independent");
            return false;
        }
        return true;
    }

    bool duplicate(Event& event) {
        if (event.kind == kTickSample) return false;
        if (options.replay) {
            if (event.flags & kLateTick) { ++late_ticks; compute_healthy = false; }
            return false;
        }
        if (event.kind == kSnapshot) {
            const std::string symbol(event.security_id);
            std::unordered_map<std::string, std::uint64_t>::iterator it = snapshot_sequences.find(symbol);
            if (it != snapshot_sequences.end() && event.sequence <= it->second) { ++duplicates; return true; }
            snapshot_sequences[symbol] = event.sequence;
            return false;
        }
        SequenceWindow& window = tick_sequences[event.channel_no - 1];
        if (window.seen.count(event.sequence)) { ++duplicates; return true; }
        if (event.sequence < window.highest) {
            ++late_ticks;
            compute_healthy = false;
            event.flags |= kLateTick;
        }
        if (event.sequence > window.highest) window.highest = event.sequence;
        window.seen.insert(event.sequence);
        window.order.push_back(event.sequence);
        if (window.order.size() > 8192) {
            window.seen.erase(window.order.front());
            window.order.pop_front();
        }
        return false;
    }

    void remove_sampling_timer(const std::string& symbol) {
        std::map<std::string, TimedTick>::iterator last = last_ticks.find(symbol);
        if (last == last_ticks.end()) return;
        sampling_due.erase(std::make_pair(last->second.due_ns, symbol));
        last_ticks.erase(last);
    }

    void schedule_tick(const Event& event, std::uint64_t origin_ns) {
        if (!options.tick_sampling || options.replay ||
            (event.flags & static_cast<std::uint8_t>(kLateTick)) != 0U)
            return;

        const std::string symbol(event.security_id);
        remove_sampling_timer(symbol);
        const std::uint64_t due_ns =
            add_ns_saturated(event.receive_mono_ns, 100000U);
        last_ticks[symbol] = TimedTick(event, origin_ns, due_ns);
        sampling_due[std::make_pair(due_ns, symbol)] = symbol;
    }

    void emit_sample(const TimedTick& timer) {
        Event sample = timer.event;
        sample.event_id = 0U;
        sample.kind = static_cast<std::uint8_t>(kTickSample);
        sample.payload_size = 72U;
        sample.receive_mono_ns =
            add_ns_saturated(timer.event.receive_mono_ns, 101000U);
        sample.receive_realtime_ns = timer.event.receive_realtime_ns;
        sample.sequence = timer.event.sequence;
        sample.channel_no = timer.event.channel_no;
        sample.shard_id = timer.event.shard_id;
        route(sample, timer.origin_ns);
    }

    // A strict comparison preserves the distinction between exactly 100 us
    // quiet and a quiet interval greater than 100 us.
    bool advance_sampling(std::uint64_t now_ns, bool flush_all) {
        if (!options.tick_sampling || options.replay) return false;
        if (now_ns > scheduler_now_ns) scheduler_now_ns = now_ns;

        bool emitted = false;
        while (!sampling_due.empty()) {
            const std::map<std::pair<std::uint64_t, std::string>, std::string>::iterator
                first = sampling_due.begin();
            if (!flush_all && !(first->first.first < now_ns)) break;
            const std::string symbol = first->second;
            sampling_due.erase(first);
            const std::map<std::string, TimedTick>::iterator timer =
                last_ticks.find(symbol);
            if (timer == last_ticks.end()) continue;
            const TimedTick value = timer->second;
            last_ticks.erase(timer);
            emit_sample(value);
            emitted = true;
        }
        return emitted;
    }

    void route(Event event, std::uint64_t origin_ns) {
        if (options.replay && event.kind == kTick &&
            event.shard_id == kUnassignedShard) {
            ++routing_errors;
            compute_healthy = false;
            fail("SSE replay tick lacks its persisted shard assignment");
            return;
        }
        if (!valid_event(event)) {
            ++routing_errors; compute_healthy = false; journal_healthy = false;
            fail("invalid SSE event envelope"); return;
        }
        if (duplicate(event)) return;
        if (options.replay && event.kind == kTickSample &&
            event.shard_id == kUnassignedShard) {
            ++routing_errors; compute_healthy = false;
            fail("SSE replay tick lacks its persisted shard assignment"); return;
        }
        const std::string symbol(event.security_id);
        std::uint32_t channel = event.channel_no, shard = kUnassignedShard;
        std::string error;
        bool mapped = false;
        if (event.kind == kTickSample) {
            // Samples are generated only after their source tick has a stable
            // route. Replay restores that exact route from the persisted
            // marker; live processing simply preserves it.
            channel = event.channel_no;
            shard = event.shard_id;
            if (options.replay)
                mapped = plan.restore(channel, symbol, shard, &error);
            else
                mapped = true;
        } else if (options.replay && event.shard_id != kUnassignedShard) {
            mapped = plan.restore(channel, symbol, event.shard_id, &error);
            shard = event.shard_id;
        } else if (event.kind == kTick) {
            mapped = plan.assign(channel, symbol, &shard, &error);
        } else {
            mapped = plan.lookup(symbol, &channel, &shard);
        }
        if (!error.empty()) {
            ++routing_errors; compute_healthy = false; journal_healthy = false;
            fail(error); return;
        }
        if (options.replay) {
            if (event.event_id != events + 1) {
                ++routing_errors; compute_healthy = false;
                fail("SSE replay event-id discontinuity"); return;
            }
            ++events;
        } else {
            event.event_id = ++events;
            event.channel_no = static_cast<std::uint16_t>(mapped ? channel : 0);
            event.shard_id = mapped ? shard : kUnassignedShard;
        }
        if (!options.replay && event.kind == kTick)
            schedule_tick(event, origin_ns);
        // Neither queue waits for the other consumer. Once a journal record
        // is lost, stop appending the discontinuous suffix and expose failure.
        if (!options.replay && journal_healthy.load()) {
          bool pushed=journal.queue.push(event, origin_ns);
          while(!pushed && journal_healthy.load() && event.receive_realtime_ns < options.history_warmup_until_ns) {
            std::this_thread::yield(); pushed=journal.queue.push(event,origin_ns);
          }
          if(!pushed) {
            ++journal.overflow;
            journal_healthy = false;
            fail("SSE journal queue overflow; live shards remain independent");
          }
        }
        if (!mapped) {
            if (pending_count >= options.pending_snapshot_capacity) {
                ++pending_overflow; compute_healthy = false;
                fail("unmapped SSE snapshot buffer full; event remains in journal");
            } else {
                pending[symbol].push_back(PendingEvent(event, origin_ns));
                ++pending_count;
            }
            return;
        }
        std::unordered_map<std::string, std::deque<PendingEvent> >::iterator waiting = pending.find(symbol);
        if (waiting != pending.end()) {
            for (const PendingEvent& snapshot : waiting->second)
                deliver(snapshot.event, snapshot.origin_ns, channel, shard);
            pending_count -= waiting->second.size();
            pending.erase(waiting);
        }
        deliver(event, origin_ns, channel, shard);
    }

    void dispatch(int cpu) {
        if (!bind(cpu)) { dispatch_done = true; return; }
        for (;;) {
            bool consumed = false;
            for (const std::unique_ptr<Producer>& producer : ingress) {
                Event event;
                std::uint64_t origin_ns = 0U;
                if (producer->queue.pop(&event, &origin_ns)) {
                    if(event.kind==0 && event.trading_day==0) {
                        for(const auto& worker:workers)
                            while(!worker->queue.push(event)) std::this_thread::yield();
                        while(!journal.queue.push(event)) std::this_thread::yield();
                        consumed=true;
                        continue;
                    }
                    const std::uint64_t dispatch_ns = monotonic_now_ns();
                    if (origin_ns != 0U && event.receive_realtime_ns >= options.latency_start_realtime_ns)
                        ingress_latency.observe(elapsed_ns(dispatch_ns, origin_ns));
                    advance_sampling(event.receive_mono_ns, false);
                    route(event, origin_ns);
                    consumed = true;
                }
            }
            if (!consumed) {
                if (!accepting.load(std::memory_order_acquire)) {
                    // A producer can publish its last event after the empty
                    // scan but before stop(). Recheck after observing stop.
                    bool remaining=false;
                    for(const auto& producer:ingress) remaining=remaining || producer->queue.size()!=0;
                    if(!remaining) break;
                    continue;
                }
                // Wall-clock advancement is intentionally limited to an idle
                // dispatcher. Historical/replay callers advance from source
                // timestamps and are flushed explicitly at stop.
                if (options.live_clock &&
                    advance_sampling(monotonic_now_ns(), false)) {
                    continue;
                }
                _mm_pause();
            }
        }
        advance_sampling(scheduler_now_ns, true);
        if (pending_count != 0) compute_healthy = false;
        dispatch_done.store(true, std::memory_order_release);
    }
};

EventPipeline::EventPipeline(const PipelineOptions& options) : impl_(new Impl(options)) {}
EventPipeline::~EventPipeline() { stop(); }

bool EventPipeline::start(const std::vector<EventConsumer>& consumers,
                          const EventConsumer& journal, const std::vector<int>& cpu_ids,
                          std::string* error) {
    Impl& p = *impl_;
    if (p.started || consumers.size() != p.workers.size() || !journal ||
        (!cpu_ids.empty() && cpu_ids.size() != p.workers.size() + 2)) {
        if (error) *error = "invalid SSE pipeline startup configuration";
        return false;
    }
    for (const EventConsumer& consumer : consumers) if (!consumer) {
        if (error) *error = "missing SSE shard callback";
        return false;
    }
    p.started = true;
    p.accepting = true;
    p.journal.callback = journal;
    try {
        p.journal.thread = std::thread(&Impl::consume, &p, &p.journal, cpu_ids.empty() ? -1 : cpu_ids[1], true);
        for (std::size_t i = 0; i < consumers.size(); ++i) {
            p.workers[i]->callback = consumers[i];
            p.workers[i]->thread = std::thread(&Impl::consume, &p, p.workers[i].get(),
                                              cpu_ids.empty() ? -1 : cpu_ids[i + 2], false);
        }
        p.dispatcher = std::thread(&Impl::dispatch, &p, cpu_ids.empty() ? -1 : cpu_ids[0]);
    } catch (const std::exception& exception) {
        p.fail(exception.what()); p.accepting = false; p.dispatch_done = true;
        stop(); if (error) *error = this->error(); return false;
    }
    while (p.ready.load() < p.workers.size() + 2) std::this_thread::yield();
    if (!p.compute_healthy || !p.journal_healthy) {
        stop(); if (error) *error = this->error(); return false;
    }
    return true;
}

bool EventPipeline::submit(std::size_t producer, const Event& event,
                           std::uint64_t origin_ns) {
    Impl& p = *impl_;
    if (producer >= p.ingress.size() || !p.accepting.load() || event.kind==0) return false;
    if (origin_ns == 0U) origin_ns = monotonic_now_ns();
    if (!p.ingress[producer]->queue.push(event, origin_ns)) {
        if (p.options.replay || event.receive_realtime_ns < p.options.history_warmup_until_ns) return false;
        ++p.ingress[producer]->overflow; p.compute_healthy = false; p.journal_healthy = false;
        return false;
    }
    ++p.ingress[producer]->submitted;
    return true;
}

void EventPipeline::stop() {
    Impl& p = *impl_;
    if (!p.started) return;
    p.accepting.store(false, std::memory_order_release);
    if (p.dispatcher.joinable()) p.dispatcher.join();
    p.dispatch_done = true;
    if (p.journal.thread.joinable()) p.journal.thread.join();
    for (const std::unique_ptr<Worker>& worker : p.workers)
        if (worker->thread.joinable()) worker->thread.join();
}

void EventPipeline::drain() {
    Impl& p=*impl_;
    if (p.ingress.size()!=1 || !p.options.history_warmup_until_ns || p.options.live_clock)
        throw std::runtime_error("drain requires single-producer historical warmup");
    p.barrier_count=0;
    Event barrier={};
    while(!p.ingress[0]->queue.push(barrier)) std::this_thread::yield();
    while(p.barrier_count.load()<p.workers.size()+1) std::this_thread::yield();
}

PipelineStats EventPipeline::stats() const {
    const Impl& p = *impl_;
    PipelineStats s = {};
    s.events = p.events; s.duplicates = p.duplicates; s.late_ticks = p.late_ticks;
    s.journal_written = p.journal.consumed; s.journal_overflow = p.journal.overflow;
    s.journal_errors = p.journal.errors; s.journal_high_water = p.journal.queue.high_water();
    s.pending_snapshots = p.pending_count; s.pending_overflow = p.pending_overflow; s.routing_errors = p.routing_errors;
    s.ingress_latency = p.ingress_latency;
    s.compute_healthy = p.compute_healthy; s.journal_healthy = p.journal_healthy;
    s.channel_stock_counts = p.plan.counts();
    for (const std::unique_ptr<Producer>& producer : p.ingress) {
        s.submitted += producer->submitted;
        s.ingress_overflow += producer->overflow;
        s.ingress_high_water.push_back(producer->queue.high_water());
    }
    for (const std::unique_ptr<Worker>& worker : p.workers) {
        s.shard_consumed.push_back(worker->consumed); s.shard_overflow.push_back(worker->overflow);
        s.shard_errors.push_back(worker->errors); s.shard_high_water.push_back(worker->queue.high_water());
        s.shard_service_latency.push_back(worker->service_latency);
        s.shard_end_to_end_latency.push_back(worker->end_to_end_latency);
        s.shard_sample_end_to_end_latency.push_back(worker->sample_end_to_end_latency);
    }
    return s;
}

std::string EventPipeline::error() const {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    return impl_->last_error;
}
}
