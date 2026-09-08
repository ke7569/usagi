#include "sse/runtime/sse_compute_pipeline.h"
#include "sse/runtime/sse_cpu_affinity.h"
#include "sse/runtime/sse_shard_plan.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <immintrin.h>

namespace sse_stream {
namespace {

const std::uint64_t kGapNs = 5000ULL;
const std::uint64_t kOpenUs = 34200000000ULL;
const std::uint64_t kEndUs = 86400000000ULL;
const std::size_t kNoRoute = std::numeric_limits<std::size_t>::max();

std::size_t checked_capacity(std::size_t value) {
    if (value < 1 || value > 1048576)
        throw std::runtime_error("SSE pipeline capacities must be in [1,1048576]");
    return value;
}

// One producer per book shard. A slot is published only after its complete
// POD tick/marker is copied; the consumer releases it before the next reuse.
template<class T> class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity)
        : slots_(capacity), mask_(capacity - 1), power_two_((capacity & (capacity - 1)) == 0),
          write_(0), read_(0) {}
    bool push(const T& value) {
        const std::size_t position = write_.load(std::memory_order_relaxed);
        if (position - read_.load(std::memory_order_acquire) >= slots_.size()) return false;
        slots_[index(position)] = value;
        write_.store(position + 1, std::memory_order_release);
        return true;
    }
    bool pop(T* value) {
        const std::size_t position = read_.load(std::memory_order_relaxed);
        if (position == write_.load(std::memory_order_acquire)) return false;
        *value = slots_[index(position)];
        read_.store(position + 1, std::memory_order_release);
        return true;
    }
private:
    std::vector<T> slots_;
    std::size_t mask_;
    bool power_two_;
    std::atomic<std::size_t> write_;
    char separation_[128];
    std::atomic<std::size_t> read_;
    std::size_t index(std::size_t value) const {
        return power_two_ ? (value & mask_) : (value % slots_.size());
    }
};

// Only accepted batch work crosses this queue. The short mutex protects a
// preallocated ring and never covers factors, inference or user callbacks.
template<class T> class ReadyQueue {
public:
    explicit ReadyQueue(std::size_t capacity)
        : slots_(capacity + 1), write_(0), read_(0) {}
    bool push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t next = (write_ + 1) % slots_.size();
        if (next == read_) return false;
        slots_[write_] = value;
        write_ = next;
        return true;
    }
    bool pop(T* value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_ == write_) return false;
        *value = slots_[read_];
        read_ = (read_ + 1) % slots_.size();
        return true;
    }
private:
    std::vector<T> slots_;
    std::size_t write_, read_;
    std::mutex mutex_;
};

int security_number(const char* code) {
    int number = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        if (code[i] < '0' || code[i] > '9') return -1;
        number = number * 10 + code[i] - '0';
    }
    return code[6] == '\0' ? number : -1;
}

Provenance source(const deepwin_market_data::StreamEvent& event,
                  std::uint32_t channel, std::uint64_t sequence, std::size_t offset) {
    Provenance value;
    value.processing_contract = kHardwareBatchV3;
    value.stream_kind = event.kind;
    value.stream_sequence = event.sequence;
    value.monotonic_ns = event.monotonic_ns;
    value.realtime_ns = event.realtime_ns;
    value.receive_batch = event.receive_batch;
    value.batch_index = event.batch_index;
    value.batch_size = event.batch_size;
    value.stream_channel_id = event.channel_id;
    value.wire_channel_no = channel;
    value.wire_sequence = sequence;
    value.record_offset = offset;
    value.source_ipv4 = event.source_ipv4;
    value.source_port = event.source_port;
    value.timestamp_flags = event.timestamp_flags;
    value.hardware_ns = event.hardware_ns;
    value.hardware_clock_index = event.hardware_clock_index;
    return value;
}

const Provenance& output_source(const Output& output) {
    return output.kind == kTickOutput ? output.tick.provenance : output.snapshot.provenance;
}

bool source_less(const Output& left, const Output& right) {
    const Provenance& a = output_source(left);
    const Provenance& b = output_source(right);
    if (a.stream_sequence != b.stream_sequence) return a.stream_sequence < b.stream_sequence;
    return a.record_offset < b.record_offset;
}

void stamp(Provenance* value, const BatchEndOutput& marker) {
    value->batch_id = marker.batch_id;
    value->batch_emitted_ns = marker.emitted_monotonic_ns;
    value->batch_close_reason = marker.reason;
}

void check_metadata(const sse_tick::DailyStaticMetadataMap& metadata) {
    if (metadata.empty()) throw std::runtime_error("SSE pipeline requires static metadata");
    std::uint32_t date = 0;
    for (const auto& entry : metadata) {
        const auto& row = entry.second;
        if (!row.complete() || !std::isfinite(row.prev_turnover) ||
            !std::isfinite(row.avg_amount) || !std::isfinite(row.turnover_threshold) ||
            !std::isfinite(row.free_share) || !std::isfinite(row.pre_close) ||
            !std::isfinite(row.limit_price) || !std::isfinite(row.stop_price))
            throw std::runtime_error("incomplete or non-finite SSE metadata: " + entry.first);
        if (date && date != row.date) throw std::runtime_error("mixed SSE metadata trading dates");
        date = row.date;
    }
}

}  // namespace

struct ComputePipeline::Impl {
    struct SnapshotRecord { sse_live::Snapshot snapshot; Provenance provenance; };
    struct Instrument {
        std::string code;
        // Only the assigned book shard may access these fields.
        sse_tick::OrderBook book;
        sse_tick::FactorState factors;
        sse_live_sampling::TickSampleGate gate;
        sse_live::DecodedTick pending_tick;
        Provenance pending_source;
        bool pending_changed;
        // Only the frontend accesses snapshot state and routing.
        sse_live::Snapshot previous_snapshot;
        bool have_snapshot;
        std::vector<SnapshotRecord> snapshots;
        std::size_t shard;
        std::uint32_t channel;
        std::size_t tick_subscription, snapshot_subscription;
        std::uint64_t next_work;
        std::vector<float> auction59;
        // Only this stock's fixed inference owner accesses model state.
        sse_hybrid_model::State model_state;
        std::uint64_t expected_work;
        Instrument(const std::string& id, const sse_tick::DailyStaticMetadata& metadata)
            : code(id), book(id), factors(), gate(), pending_tick(), pending_source(),
              pending_changed(false), previous_snapshot(), have_snapshot(false),
              snapshots(), shard(kNoRoute), channel(0), tick_subscription(kNoRoute),
              snapshot_subscription(kNoRoute), next_work(0),
              auction59(), model_state(), expected_work(0) {
            factors.set_static_metadata(metadata);
        }
    };
    struct Subscription {
        bool open, have_hardware;
        std::uint64_t last_hw, last_mono;
        BatchEndOutput marker;
        std::vector<std::size_t> touched;
        // Bit 1: book updates. Bit 2: snapshots. Different subscriptions
        // cannot release each other's still-open sampling candidates.
        std::vector<unsigned char> activity;
        explicit Subscription(std::size_t stocks)
            : open(false), have_hardware(false), last_hw(0), last_mono(0), marker(),
              touched(), activity(stocks, 0) { touched.reserve(stocks); }
    };
    struct Batch;
    struct Work {
        std::size_t instrument;
        std::uint64_t ordinal;
        Batch* batch;
        std::vector<Output> outputs;
        bool completed;
        bool tick_cut;
        std::size_t reserved_rows;
        Work(std::size_t index, std::uint64_t sequence, Batch* parent)
            : instrument(index), ordinal(sequence), batch(parent), outputs(), completed(false),
              tick_cut(false), reserved_rows(0) {}
    };
    struct Batch {
        BatchEndOutput marker;
        std::vector<std::unique_ptr<Work> > works;
        std::vector<std::vector<Work*> > by_shard;
        std::vector<Work*> direct;
        std::size_t completed;
        explicit Batch(std::size_t shards) : marker(), works(), by_shard(shards), direct(), completed(0) {}
    };
    struct Command {
        enum Kind { Tick = 1, BatchEnd = 2 } kind;
        std::size_t instrument;
        sse_live::DecodedTick tick;
        Provenance provenance;
        Batch* batch;
        Command() : kind(Tick), instrument(0), tick(), provenance(), batch(0) {}
    };
    struct Counter {
        std::atomic<std::uint64_t> value;
        char separation[128];
        Counter() : value(0), separation() {}
        void increment() {
            // Exactly one worker writes each counter. Avoid a contended RMW
            // on a cache line shared by independent book/L3 domains.
            value.store(value.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    };

    PipelineConfig config;
    bool parallel;
    const sse_hybrid_model::Model* model;
    const std::string exchange;
    bool factors_only;
    OutputCallback callback;
    sse_pipeline::ShardPlan routes;
    std::vector<std::unique_ptr<Instrument> > instruments;
    std::vector<int> instrument_lookup;
    std::map<std::uint32_t, std::unique_ptr<Subscription> > subscriptions;
    Subscription* current_subscription;
    std::uint64_t channel_sequence[65536];
    std::vector<sse_live::DecodedTick> decode_scratch;
    std::vector<std::unique_ptr<SpscQueue<Command> > > book_queues;
    std::vector<std::unique_ptr<ReadyQueue<Work*> > > inference_queues;
    ReadyQueue<Work*> completions;
    std::deque<std::unique_ptr<Batch> > pending;
    std::size_t pending_work, pending_snapshots;
    sse_cpu::Lease lease;
    std::vector<std::thread> threads;
    std::vector<int> book_cpus, inference_cpus;
    std::atomic<bool> cancelled;
    std::atomic<unsigned> started;
    std::vector<std::unique_ptr<Counter> > applied_ticks, accepted_samples, inferred_samples;
    std::atomic<std::size_t> retained_rows, retained_row_high_water;
    std::atomic<std::uint64_t> closed_batches;
    mutable std::mutex routing_mutex;
    mutable std::mutex error_mutex;
    std::string error;
    std::thread::id owner;
    bool have_owner, finished, have_clock, have_source;
    std::uint64_t next_batch, last_sequence, last_observed_mono;
    std::int32_t hardware_clock_index;

    Impl(const sse_tick::DailyStaticMetadataMap& metadata,
         const sse_hybrid_model::Model* supplied_model, bool only_factors,
         const OutputCallback& supplied_callback, const Auction59Provider& auction,
         const PipelineConfig& supplied_config)
        : config(supplied_config), parallel(!config.book_cpus.empty()), model(supplied_model), exchange("sse"),
          factors_only(only_factors), callback(supplied_callback),
          routes(parallel ? config.book_cpus.size() : 1), instruments(),
          instrument_lookup(1000000, -1), subscriptions(), current_subscription(0),
          channel_sequence(), decode_scratch(65535 / 72),
          book_queues(), inference_queues(), completions(checked_capacity(config.output_capacity)), pending(),
          pending_work(0), pending_snapshots(0), lease(), threads(), book_cpus(), inference_cpus(),
          cancelled(false), started(0), applied_ticks(), accepted_samples(), inferred_samples(),
          retained_rows(0), retained_row_high_water(0), closed_batches(0), routing_mutex(),
          error_mutex(), error(), owner(), have_owner(false), finished(false),
          have_clock(false), have_source(false), next_batch(1),
          last_sequence(0), last_observed_mono(0), hardware_clock_index(-1) {
        if (config.contract != kHardwareBatchV3 ||
            config.book_cpus.empty() != config.inference_cpus.empty())
            throw std::runtime_error("SSE hardware pipeline requires both worker CPU lists or serial mode");
        if (config.ingress_capacity < 1 || config.ingress_capacity > 1048576 ||
            config.inference_capacity < 1 || config.inference_capacity > 1048576 ||
            config.output_capacity < 1 || config.output_capacity > 1048576)
            throw std::runtime_error("SSE pipeline capacities must be in [1,1048576]");
        check_metadata(metadata);
        for (const auto& entry : metadata) {
            std::string code;
            if (!sse_tick::normalize_sse_security_id(entry.first, &code))
                throw std::runtime_error("invalid SSE pipeline security: " + entry.first);
            const int key = security_number(code.c_str());
            if (key < 0 || instrument_lookup[key] >= 0)
                throw std::runtime_error("invalid or duplicate SSE pipeline security: " + code);
            instrument_lookup[key] = static_cast<int>(instruments.size());
            instruments.emplace_back(new Instrument(code, entry.second));
            if (!factors_only) {
                std::string detail;
                std::vector<float>& values = instruments.back()->auction59;
                if (!auction || !auction(code, kOpenUs, &values, &detail) || values.size() != 59)
                    throw std::runtime_error("SSE Auction59 provider rejected instrument: " + code + " " + detail);
                for (float value : values) if (!std::isfinite(value))
                    throw std::runtime_error("non-finite SSE Auction59 factor");
            }
        }
        for (std::size_t i = 0; i < (parallel ? config.book_cpus.size() : 1); ++i)
            applied_ticks.emplace_back(new Counter);
        for (std::size_t i = 0; i < (parallel ? config.inference_cpus.size() : 1); ++i) {
            accepted_samples.emplace_back(new Counter);
            inferred_samples.emplace_back(new Counter);
        }
        if (!parallel) return;
        std::vector<int> requested(config.book_cpus);
        requested.insert(requested.end(), config.inference_cpus.begin(), config.inference_cpus.end());
        std::string detail;
        if (!lease.acquire(requested, &detail)) throw std::runtime_error(detail);
        for (std::size_t i = 0; i < config.book_cpus.size(); ++i) {
            book_cpus.push_back(lease.cpus()[i].id);
            book_queues.emplace_back(new SpscQueue<Command>(config.ingress_capacity));
        }
        for (std::size_t i = 0; i < config.inference_cpus.size(); ++i) {
            inference_cpus.push_back(lease.cpus()[book_cpus.size() + i].id);
            inference_queues.emplace_back(new ReadyQueue<Work*>(config.inference_capacity));
        }
        try {
            for (std::size_t i = 0; i < book_cpus.size(); ++i)
                threads.emplace_back([this, i]() { book_loop(i); });
            for (std::size_t i = 0; i < inference_cpus.size(); ++i)
                threads.emplace_back([this, i]() { inference_loop(i); });
            while (started.load(std::memory_order_acquire) < requested.size()) {
                check_error();
                std::this_thread::yield();
            }
            check_error();
        } catch (...) { stop_workers(); throw; }
    }

    ~Impl() { stop_workers(); }

    void stop_workers() {
        cancelled.store(true, std::memory_order_release);
        for (std::thread& thread : threads) if (thread.joinable()) thread.join();
        lease.release();
    }

    void record_error(const std::string& detail) {
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (error.empty()) error = detail.empty() ? "SSE pipeline worker failure" : detail;
        }
        cancelled.store(true, std::memory_order_release);
    }

    void check_error() const {
        if (!cancelled.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!error.empty()) throw std::runtime_error(error);
    }

    void claim_owner() {
        if (!have_owner) { owner = std::this_thread::get_id(); have_owner = true; }
        if (owner != std::this_thread::get_id()) {
            record_error("SSE input and output polling require one owner thread");
            throw std::runtime_error("SSE input and output polling require one owner thread");
        }
    }

    int lookup(const char* code) const {
        const int key = security_number(code);
        return key < 0 ? -1 : instrument_lookup[key];
    }

    void touch(std::size_t index, unsigned char activity) {
        if (!current_subscription->activity[index]) current_subscription->touched.push_back(index);
        current_subscription->activity[index] |= activity;
    }

    void reserve_row() {
        std::size_t count = retained_rows.load(std::memory_order_relaxed);
        do {
            if (count >= config.output_capacity)
                throw std::runtime_error("SSE retained sample capacity exceeded; prediction invalid");
        } while (!retained_rows.compare_exchange_weak(count, count + 1,
                   std::memory_order_relaxed, std::memory_order_relaxed));
        std::size_t peak = retained_row_high_water.load(std::memory_order_relaxed);
        while (peak < count + 1 && !retained_row_high_water.compare_exchange_weak(
            peak, count + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    void submit_tick(const sse_live::DecodedTick& tick, const Provenance& provenance) {
        if (tick.channel_no == 0 || tick.channel_no >= 65536 || tick.tick_index == 0)
            throw std::runtime_error("SSE hardware-v3 expects a valid wire channel and nonzero TickIndex");
        std::uint64_t& previous = channel_sequence[tick.channel_no];
        if (previous) {
            // Packet replay is ignored consistently with the software contract.
            if (tick.tick_index <= previous) return;
            if (tick.tick_index - previous != 1)
                throw std::runtime_error("SSE tick sequence gap on wire channel");
        }
        previous = tick.tick_index;
        const int found = lookup(tick.security_id);
        if (found < 0) return;
        if (tick.channel_no > 6)
            throw std::runtime_error("SSE watched A-share stock expects wire ChannelNo 1..6");
        Instrument& instrument = *instruments[found];
        if (instrument.tick_subscription == kNoRoute) instrument.tick_subscription = provenance.stream_channel_id;
        else if (instrument.tick_subscription != provenance.stream_channel_id)
            throw std::runtime_error("SSE stock changed tick UDP subscription");
        if (instrument.shard == kNoRoute) {
            std::uint32_t shard = 0;
            std::string detail;
            std::lock_guard<std::mutex> lock(routing_mutex);
            if (!routes.assign(tick.channel_no, instrument.code, &shard, &detail))
                throw std::runtime_error(detail);
            instrument.shard = shard;
            instrument.channel = tick.channel_no;
        } else if (instrument.channel != tick.channel_no) {
            throw std::runtime_error("SSE stock changed wire channel after shard assignment");
        }
        touch(static_cast<std::size_t>(found), 1);
        Command command;
        command.instrument = static_cast<std::size_t>(found);
        command.tick = tick;
        command.provenance = provenance;
        if (parallel) {
            if (!book_queues[instrument.shard]->push(command))
                throw std::runtime_error("SSE book ingress queue overflow; prediction invalid");
        } else apply_tick(command);
    }

    void submit_snapshot(const sse_live::Snapshot& snapshot, const Provenance& provenance) {
        if (!snapshot.sequence) throw std::runtime_error("SSE snapshot has invalid wire sequence");
        if (snapshot.security_id.size() != 6) return;
        const int found = lookup(snapshot.security_id.c_str());
        if (found < 0) return;
        Instrument& instrument = *instruments[found];
        if (instrument.snapshot_subscription == kNoRoute) instrument.snapshot_subscription = provenance.stream_channel_id;
        else if (instrument.snapshot_subscription != provenance.stream_channel_id)
            throw std::runtime_error("SSE stock changed snapshot UDP subscription");
        if (++pending_snapshots > config.ingress_capacity)
            throw std::runtime_error("SSE open-batch snapshot capacity exceeded; prediction invalid");
        reserve_row();
        SnapshotRecord record;
        record.snapshot = snapshot;
        record.provenance = provenance;
        instruments[found]->snapshots.push_back(std::move(record));
        touch(static_cast<std::size_t>(found), 2);
    }

    void parse_datagram(const deepwin_market_data::StreamEvent& event) {
        if (!event.data || !event.size || event.size > 65535)
            throw std::runtime_error("SSE datagram is empty or too large");
        if (sse_live::is_primary_heartbeat(event.data, event.size)) return;
        if (event.size % 72 == 0 && event.data[8] == 0x3e) {
            std::size_t count = 0, bad = 0;
            sse_live::TickDecodeError detail;
            const bool decoded = sse_live::decode_primary_tick_packet(event.data, event.size,
                decode_scratch.data(), decode_scratch.size(), &count, &detail, &bad, false);
            if (!decoded && detail != sse_live::kTickDecodeAbi)
                throw std::runtime_error(std::string("SSE tick packet rejected at ") +
                    std::to_string(bad) + ": " + sse_live::tick_decode_error_text(detail));
            if (decoded) {
                for (std::size_t i = 0; i < count; ++i)
                    submit_tick(decode_scratch[i], source(event, decode_scratch[i].channel_no,
                        decode_scratch[i].tick_index, i * 72));
                return;
            }
        }
        std::size_t offset = 0;
        while (offset < event.size) {
            const std::size_t left = event.size - offset;
            const unsigned char* bytes = event.data + offset;
            if (left >= 72 && bytes[8] == 0x3e) {
                sse_live::DecodedTick tick;
                sse_live::TickDecodeError detail;
                if (!sse_live::decode_primary_tick_fast(bytes, 72, &tick, &detail, false))
                    throw std::runtime_error(sse_live::tick_decode_error_text(detail));
                submit_tick(tick, source(event, tick.channel_no, tick.tick_index, offset));
                offset += 72;
            } else if (left >= 440 && bytes[8] == 0x27) {
                sse_live::Snapshot snapshot;
                std::string detail;
                if (!sse_live::decode_primary_snapshot(bytes, 440, &snapshot, &detail, false))
                    throw std::runtime_error(detail);
                submit_snapshot(snapshot, source(event, event.channel_id, snapshot.sequence, offset));
                offset += 440;
            } else {
                throw std::runtime_error("unknown or truncated SSE wire record");
            }
        }
    }

    sse_live_sampling::TickCut cut(const Instrument& instrument) const {
        sse_tick::Level bids[1] = {}, asks[1] = {};
        instrument.book.snapshot(bids, asks, 1);
        sse_live_sampling::TickCut value;
        value.cut_index = instrument.pending_tick.tick_index;
        value.exchange_time_of_day_micros = instrument.pending_tick.time_of_day_micros;
        value.valid_book = bids[0].price_raw > 0 && asks[0].price_raw > 0 &&
            bids[0].quantity > 0 && asks[0].quantity > 0;
        value.mid_price = value.valid_book ?
            (static_cast<double>(bids[0].price_raw) + asks[0].price_raw) / 2000.0 :
            instrument.book.last_trade_price();
        value.cumulative_turnover = instrument.book.total_trade_turnover();
        value.cumulative_volume = static_cast<std::int64_t>(instrument.book.total_trade_qty() / 1000);
        value.continuous_trading = value.exchange_time_of_day_micros >= kOpenUs &&
            value.exchange_time_of_day_micros < kEndUs;
        return value;
    }

    void apply_tick(const Command& command, std::size_t shard = 0) {
        Instrument& instrument = *instruments[command.instrument];
        const sse_tick::ApplyResult applied = instrument.book.apply(command.tick);
        if (!applied.accepted && command.tick.event_type != sse_tick::kStatus)
            throw std::runtime_error(std::string("SSE order-book rejected ") + applied.reason);
        applied_ticks[shard]->increment();
        if (!applied.accepted || !applied.book_changed) return;
        instrument.pending_tick = command.tick;
        instrument.pending_source = command.provenance;
        instrument.pending_changed = true;
        if (!instrument.gate.initialized()) {
            const sse_live_sampling::TickCut value = cut(instrument);
            sse_live_sampling::SampleDecision decision;
            std::string detail;
            if (!instrument.gate.observe_batch_end(value, instrument.factors.turnover_threshold(),
                                                   &decision, &detail))
                throw std::runtime_error("SSE opening window failed: " + detail);
            if (instrument.gate.initialized()) {
                instrument.factors.seed_window(value.mid_price, value.exchange_time_of_day_micros,
                    static_cast<double>(instrument.book.total_trade_qty()), value.cumulative_turnover);
                instrument.book.take_flow_window();
            }
        }
    }

    void build_tick(Work* work) {
        if (!work->tick_cut) return;
        Instrument& instrument = *instruments[work->instrument];
        if (!instrument.pending_changed) return;
        instrument.pending_changed = false;
        const sse_live_sampling::TickCut value = cut(instrument);
        sse_live_sampling::SampleDecision decision;
        std::string detail;
        if (!instrument.gate.observe_batch_end(value, instrument.factors.turnover_threshold(),
                                               &decision, &detail))
            throw std::runtime_error("SSE sample gate rejected cut: " + detail);
        if (!instrument.gate.initialized() || !decision.accepted) return;
        reserve_row();
        ++work->reserved_rows;
        Output output;
        output.kind = kTickOutput;
        output.tick.factors = instrument.factors.build(instrument.book, value.exchange_time_of_day_micros);
        sse_live::materialize_tick(instrument.pending_tick, &output.tick.event);
        sse_tick::Level bids[10] = {}, asks[10] = {};
        if (!instrument.book.snapshot(bids, asks, 10))
            throw std::runtime_error("SSE order-book snapshot failed");
        output.tick.bid_levels.assign(bids, bids + 10);
        output.tick.ask_levels.assign(asks, asks + 10);
        output.tick.last_trade_price = instrument.book.last_trade_price();
        output.tick.total_trade_volume = instrument.book.total_trade_qty() / 1000.0;
        output.tick.total_trade_turnover = instrument.book.total_trade_turnover();
        output.tick.sample_decision = decision;
        output.tick.provenance = instrument.pending_source;
        stamp(&output.tick.provenance, work->batch->marker);
        work->outputs.push_back(std::move(output));
    }

    void build_snapshots(Work* work) {
        Instrument& instrument = *instruments[work->instrument];
        const std::size_t reserved = instrument.snapshots.size();
        std::size_t accepted = 0;
        for (const SnapshotRecord& record : instrument.snapshots) {
            const sse_live::Snapshot& snapshot = record.snapshot;
            if (!sse_snapshot36::valid(snapshot)) continue;
            if (instrument.have_snapshot &&
                snapshot.time_of_day_micros <= instrument.previous_snapshot.time_of_day_micros) continue;
            if (!instrument.have_snapshot) {
                instrument.previous_snapshot = snapshot;
                instrument.have_snapshot = true;
                continue;
            }
            const sse_live::Snapshot previous = instrument.previous_snapshot;
            if (snapshot.volume < previous.volume || snapshot.turnover < previous.turnover)
                throw std::runtime_error("SSE snapshot cumulative values regressed within trading day");
            instrument.previous_snapshot = snapshot;
            if (snapshot.time_of_day_micros < kSnapshotGenerateStartMicros ||
                snapshot.time_of_day_micros >= kSnapshotGenerateEndMicros) continue;
            Output output;
            output.kind = kSnapshotOutput;
            output.snapshot.snapshot = snapshot;
            output.snapshot.snapshot36 = sse_snapshot36::build(previous, snapshot);
            if (output.snapshot.snapshot36.size() != 36)
                throw std::runtime_error("SSE Snapshot36 factor count mismatch");
            output.snapshot.auction59 = instrument.auction59;
            output.snapshot.provenance = record.provenance;
            stamp(&output.snapshot.provenance, work->batch->marker);
            work->outputs.push_back(std::move(output));
            ++accepted;
        }
        std::vector<SnapshotRecord>().swap(instrument.snapshots);
        work->reserved_rows += accepted;
        retained_rows.fetch_sub(reserved - accepted, std::memory_order_relaxed);
    }

    void infer(Work* work) {
        Instrument& instrument = *instruments[work->instrument];
        if (work->ordinal != instrument.expected_work++)
            throw std::runtime_error("SSE per-stock inference work arrived out of order");
        std::stable_sort(work->outputs.begin(), work->outputs.end(), source_less);
        for (Output& output : work->outputs) {
            if (cancelled.load(std::memory_order_acquire)) return;
            const std::size_t shard = work->instrument % accepted_samples.size();
            accepted_samples[shard]->increment();
            if (factors_only) continue;
            std::string detail;
            if (output.kind == kTickOutput) {
                output.tick.prediction_valid = model->on_tick(output.tick.factors.values, exchange,
                    output.tick.event.time_of_day_micros, &instrument.model_state,
                    &output.tick.prediction, &detail);
                if (!output.tick.prediction_valid)
                    throw std::runtime_error("SSE tick model rejected factors: " + detail);
                if (!output.tick.factors.validity.complete) {
                    output.tick.prediction.selected = false;
                    output.tick.prediction.selected_source = sse_hybrid_model::kNoSource;
                    output.tick.prediction.selected_pred = 0;
                }
            } else {
                std::vector<float> enhanced(output.snapshot.snapshot36);
                enhanced.insert(enhanced.end(), instrument.auction59.begin(), instrument.auction59.end());
                output.snapshot.prediction_valid = model->on_snapshot(output.snapshot.snapshot36,
                    enhanced, exchange, output.snapshot.snapshot.time_of_day_micros,
                    &instrument.model_state, &output.snapshot.prediction, &detail);
                if (!output.snapshot.prediction_valid)
                    throw std::runtime_error("SSE snapshot model rejected factors: " + detail);
            }
            inferred_samples[shard]->increment();
        }
        if (!completions.push(work))
            throw std::runtime_error("SSE output queue overflow; prediction invalid");
    }

    void ready(Work* work) {
        if (parallel) {
            if (!inference_queues[work->instrument % inference_queues.size()]->push(work))
                throw std::runtime_error("SSE inference queue overflow; prediction invalid");
        } else infer(work);
    }

    void close_batch(Subscription& subscription, std::uint64_t now,
                     sse_live_sampling::BatchCloseReason reason) {
        if (!subscription.open) return;
        if (pending.size() >= config.output_capacity || subscription.touched.size() > config.output_capacity - pending_work)
            throw std::runtime_error("SSE pending batch capacity exceeded; prediction invalid");
        std::unique_ptr<Batch> batch(new Batch(parallel ? book_queues.size() : 1));
        batch->marker = subscription.marker;
        batch->marker.emitted_monotonic_ns = now;
        batch->marker.reason = reason;
        batch->marker.candidate_count = static_cast<std::uint32_t>(subscription.touched.size());
        batch->works.reserve(subscription.touched.size());
        for (std::size_t index : subscription.touched) {
            Instrument& instrument = *instruments[index];
            batch->works.emplace_back(new Work(index, instrument.next_work++, batch.get()));
            Work* work = batch->works.back().get();
            work->tick_cut = (subscription.activity[index] & 1) != 0;
            if (subscription.activity[index] & 2) {
                pending_snapshots -= instrument.snapshots.size();
                build_snapshots(work);
            }
            if (instrument.shard == kNoRoute) batch->direct.push_back(work);
            else batch->by_shard[instrument.shard].push_back(work);
            subscription.activity[index] = 0;
        }
        pending_work += batch->works.size();
        Batch* published = batch.get();
        pending.push_back(std::move(batch));
        // This is the explicit input BatchEnd. No worker may construct a tick
        // factor row until it consumes this marker after all preceding ticks.
        for (std::size_t shard = 0; shard < published->by_shard.size(); ++shard) {
            if (published->by_shard[shard].empty()) continue;
            if (parallel) {
                Command marker;
                marker.kind = Command::BatchEnd;
                marker.batch = published;
                if (!book_queues[shard]->push(marker))
                    throw std::runtime_error("SSE book BatchEnd queue overflow; prediction invalid");
            } else {
                for (Work* work : published->by_shard[shard]) { build_tick(work); ready(work); }
            }
        }
        for (Work* work : published->direct) ready(work);
        subscription.touched.clear();
        subscription.open = false;
        ++closed_batches;
    }

    void dispatch_outputs() {
        check_error();
        Work* work = 0;
        while (completions.pop(&work)) {
            work->completed = true;
            ++work->batch->completed;
        }
        while (!pending.empty() && pending.front()->completed == pending.front()->works.size()) {
            Batch& batch = *pending.front();
            std::vector<const Output*> outputs;
            for (const auto& item : batch.works)
                for (const Output& output : item->outputs) outputs.push_back(&output);
            std::stable_sort(outputs.begin(), outputs.end(), [](const Output* a, const Output* b) {
                return source_less(*a, *b);
            });
            for (const Output* output : outputs) {
                check_error();
                if (output->kind == kTickOutput ? output->tick.prediction_valid : output->snapshot.prediction_valid)
                    ++batch.marker.prediction_count;
                callback(*output);
            }
            Output marker;
            marker.kind = kBatchEndOutput;
            marker.batch_end = batch.marker;
            callback(marker);
            std::size_t released = 0;
            for (const auto& item : batch.works) released += item->reserved_rows;
            retained_rows.fetch_sub(released, std::memory_order_relaxed);
            pending_work -= batch.works.size();
            pending.pop_front();
        }
    }

    void on_event(const deepwin_market_data::StreamEvent& event) {
        claim_owner();
        check_error();
        if (finished) throw std::runtime_error("SSE input after pipeline finish");
        try {
            dispatch_outputs();
            if (have_source && (event.sequence <= last_sequence || event.monotonic_ns < last_observed_mono))
                throw std::runtime_error("SSE source sequence or monotonic timestamp moved backwards");
            have_source = true;
            last_sequence = event.sequence;
            last_observed_mono = event.monotonic_ns;
            if (event.kind == deepwin_market_data::kIdleEvent) {
                if (event.data || event.size) throw std::runtime_error("SSE idle event carries payload");
                // Compare monotonic with monotonic only. An early idle cannot
                // cut a PHC batch merely because a receive syscall returned.
                std::vector<Subscription*> eligible;
                for (auto& item : subscriptions)
                    if (item.second->open && event.monotonic_ns - item.second->last_mono >= kGapNs)
                        eligible.push_back(item.second.get());
                std::sort(eligible.begin(), eligible.end(), [](const Subscription* a, const Subscription* b) {
                    return a->marker.last_stream_sequence < b->marker.last_stream_sequence;
                });
                for (Subscription* subscription : eligible)
                    close_batch(*subscription, event.monotonic_ns, sse_live_sampling::kBatchClosedByTimer);
            } else if (event.kind == deepwin_market_data::kDatagramEvent) {
                const unsigned required = deepwin_market_data::kHardwareReceiveTimestamp |
                    deepwin_market_data::kHardwareTimestampRequested;
                if ((event.timestamp_flags & required) != required || !event.hardware_ns ||
                    event.hardware_clock_index < 0)
                    throw std::runtime_error("SSE hardware-v3 requires hardware timestamp and PHC identity on every UDP datagram");
                if (have_clock && event.hardware_clock_index != hardware_clock_index)
                    throw std::runtime_error("SSE PHC identity changed within stream");
                auto found = subscriptions.find(event.channel_id);
                if (found == subscriptions.end()) {
                    if (subscriptions.size() >= config.ingress_capacity)
                        throw std::runtime_error("SSE UDP subscription capacity exceeded");
                    subscriptions[event.channel_id].reset(new Subscription(instruments.size()));
                    found = subscriptions.find(event.channel_id);
                }
                Subscription& subscription = *found->second;
                current_subscription = &subscription;
                if (subscription.have_hardware && event.hardware_ns < subscription.last_hw)
                    throw std::runtime_error("SSE hardware receive timestamp moved backwards");
                if (subscription.open && event.hardware_ns - subscription.last_hw >= kGapNs)
                    close_batch(subscription, event.monotonic_ns, sse_live_sampling::kBatchClosedByNextEvent);
                if (!subscription.open) {
                    subscription.marker = BatchEndOutput();
                    subscription.marker.batch_id = next_batch++;
                    subscription.marker.stream_channel_id = event.channel_id;
                    subscription.open = true;
                }
                have_clock = true;
                hardware_clock_index = event.hardware_clock_index;
                subscription.have_hardware = true;
                subscription.marker.hardware_clock_index = event.hardware_clock_index;
                subscription.marker.timestamp_flags = event.timestamp_flags;
                subscription.marker.last_hardware_ns = event.hardware_ns;
                subscription.marker.last_stream_sequence = event.sequence;
                ++subscription.marker.packet_count;
                subscription.last_hw = event.hardware_ns;
                subscription.last_mono = event.monotonic_ns;
                parse_datagram(event);
            } else throw std::runtime_error("unknown SSE stream event kind");
            dispatch_outputs();
        } catch (const std::exception& exception) {
            record_error(exception.what());
            throw;
        } catch (...) {
            record_error("unknown SSE pipeline callback failure");
            throw;
        }
    }

    void poll_outputs() {
        claim_owner();
        try { dispatch_outputs(); }
        catch (const std::exception& exception) { record_error(exception.what()); throw; }
        catch (...) { record_error("unknown SSE output callback failure"); throw; }
    }

    void finish() {
        claim_owner();
        try {
            if (!finished) {
                // shutdown_flush=false: the last open batch is intentionally
                // not closed. Already published batch work must be drained.
                while (!pending.empty()) {
                    dispatch_outputs();
                    if (!pending.empty()) std::this_thread::yield();
                }
                stop_workers();
                discard_open_snapshots();
                finished = true;
            }
            check_error();
        } catch (...) {
            stop_workers();
            pending.clear();
            pending_work = 0;
            discard_open_snapshots();
            retained_rows.store(0, std::memory_order_relaxed);
            finished = true;
            throw;
        }
    }

    void discard_open_snapshots() {
        std::size_t discarded = 0;
        for (auto& instrument : instruments) {
            discarded += instrument->snapshots.size();
            std::vector<SnapshotRecord>().swap(instrument->snapshots);
        }
        const std::size_t retained = retained_rows.load(std::memory_order_relaxed);
        retained_rows.fetch_sub(std::min(discarded, retained), std::memory_order_relaxed);
        pending_snapshots = 0;
    }

    void book_loop(std::size_t shard) {
        try {
            std::string detail;
            if (!sse_cpu::bind_current_thread(book_cpus[shard], &detail)) throw std::runtime_error(detail);
            started.fetch_add(1, std::memory_order_release);
            Command command;
            while (!cancelled.load(std::memory_order_acquire)) {
                if (!book_queues[shard]->pop(&command)) { _mm_pause(); continue; }
                if (command.kind == Command::Tick) apply_tick(command, shard);
                else {
                    // Publishing the last Work permits the owner to reclaim
                    // the Batch immediately. Keep the loop bound local and do
                    // not touch the Batch again after that publication.
                    const std::size_t count = command.batch->by_shard[shard].size();
                    for (std::size_t i = 0; i < count; ++i) {
                        if (cancelled.load(std::memory_order_acquire)) return;
                        Work* work = command.batch->by_shard[shard][i];
                        build_tick(work);
                        ready(work);
                    }
                }
            }
        } catch (const std::exception& exception) { record_error(exception.what()); }
        catch (...) { record_error("unknown SSE book worker failure"); }
    }

    void inference_loop(std::size_t shard) {
        try {
            std::string detail;
            if (!sse_cpu::bind_current_thread(inference_cpus[shard], &detail)) throw std::runtime_error(detail);
            started.fetch_add(1, std::memory_order_release);
            Work* work = 0;
            while (!cancelled.load(std::memory_order_acquire)) {
                if (!inference_queues[shard]->pop(&work)) { _mm_pause(); continue; }
                infer(work);
            }
        } catch (const std::exception& exception) { record_error(exception.what()); }
        catch (...) { record_error("unknown SSE inference worker failure"); }
    }

    PipelineStats stats() const {
        PipelineStats result;
        for (const auto& count : applied_ticks) result.applied_ticks += count->value.load(std::memory_order_relaxed);
        for (const auto& count : accepted_samples) result.accepted_samples += count->value.load(std::memory_order_relaxed);
        for (const auto& count : inferred_samples) result.inferred_samples += count->value.load(std::memory_order_relaxed);
        result.closed_batches = closed_batches.load(std::memory_order_relaxed);
        result.retained_rows = retained_rows.load(std::memory_order_relaxed);
        result.retained_row_high_water = retained_row_high_water.load(std::memory_order_relaxed);
        result.book_cpus = book_cpus;
        result.inference_cpus = inference_cpus;
        {
            std::lock_guard<std::mutex> lock(routing_mutex);
            result.channel_shard_counts = routes.counts();
        }
        return result;
    }
};

ComputePipeline::ComputePipeline(const sse_tick::DailyStaticMetadataMap& metadata,
        const sse_hybrid_model::Model* model, bool factors_only, const OutputCallback& callback,
        const Auction59Provider& auction, const PipelineConfig& config)
    : impl_(new Impl(metadata, model, factors_only, callback, auction, config)) {}
ComputePipeline::~ComputePipeline() {}
void ComputePipeline::on_event(const deepwin_market_data::StreamEvent& event) { impl_->on_event(event); }
void ComputePipeline::poll_outputs() { impl_->poll_outputs(); }
void ComputePipeline::finish() { impl_->finish(); }
PipelineStats ComputePipeline::stats() const { return impl_->stats(); }

}  // namespace sse_stream
