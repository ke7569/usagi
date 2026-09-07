#include "sze/factors/sz_hp_factor_adapter.h"
#include "sze/market_data/sz_hp_latency.h"
#include "common/market_data/legacy/shsz_full_orderbook_manager.h"
#include "common/market_data/legacy/snap_generator.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <sched.h>
#include <unistd.h>

namespace {

struct Options {
    size_t iterations;
    size_t warmup;
    size_t reset_interval;
    size_t factor_iterations;
    int cpu;

    Options()
        : iterations(100000), warmup(10000), reset_interval(256),
          factor_iterations(10000), cpu(-1) {
    }
};

bool parse_size(const char* text, size_t* destination) {
    if (text == 0 || destination == 0 || text[0] == '\0') {
        return false;
    }
    char* end = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
        return false;
    }
    *destination = static_cast<size_t>(value);
    return true;
}

bool parse_options(int argc, char** argv, Options* options) {
    if (options == 0) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) {
            return false;
        }
        const std::string key(argv[i]);
        if (key == "--cpu") {
            char* end = 0;
            const long value = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < 0) {
                return false;
            }
            options->cpu = static_cast<int>(value);
            continue;
        }
        size_t* destination = 0;
        if (key == "--iterations") {
            destination = &options->iterations;
        } else if (key == "--warmup") {
            destination = &options->warmup;
        } else if (key == "--reset-interval") {
            destination = &options->reset_interval;
        } else if (key == "--factor-iterations") {
            destination = &options->factor_iterations;
        } else {
            return false;
        }
        if (!parse_size(argv[++i], destination)) {
            return false;
        }
    }
    return true;
}

bool pin_to_cpu(int cpu) {
    if (cpu < 0) {
        return true;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

void copy_text(char* destination, size_t size, const char* source) {
    if (destination == 0 || size == 0) {
        return;
    }
    std::memset(destination, 0, size);
    if (source != 0) {
        std::strncpy(destination, source, size - 1);
    }
}

LFL2OrderField make_order(uint64_t sequence, bool buy) {
    LFL2OrderField event;
    std::memset(&event, 0, sizeof(event));
    copy_text(event.InstrumentID, sizeof(event.InstrumentID), "000001");
    copy_text(event.ExchangeID, sizeof(event.ExchangeID), "SZ");
    copy_text(event.OrderTime, sizeof(event.OrderTime), "09:30:00.000");
    copy_text(event.OrderKind, sizeof(event.OrderKind), buy ? "B" : "S");
    copy_text(event.OrdType, sizeof(event.OrdType), "2");
    event.Price = buy ? 10.0 : 10.1;
    event.Volume = 100.0;
    event.ApplSeqNum = static_cast<int64_t>(sequence);
    event.BizIndex = static_cast<int64_t>(sequence);
    event.IsLast = 1;
    return event;
}

LFL2TradeField make_trade(uint64_t sequence, uint64_t bid_id, uint64_t ask_id) {
    LFL2TradeField event;
    std::memset(&event, 0, sizeof(event));
    copy_text(event.InstrumentID, sizeof(event.InstrumentID), "000001");
    copy_text(event.ExchangeID, sizeof(event.ExchangeID), "SZ");
    copy_text(event.TradeTime, sizeof(event.TradeTime), "09:30:00.000");
    copy_text(event.OrderKind, sizeof(event.OrderKind), "F");
    event.Price = 10.05;
    event.Volume = 100.0;
    event.BidApplSeqNum = static_cast<int64_t>(bid_id);
    event.OfferApplSeqNum = static_cast<int64_t>(ask_id);
    event.ApplSeqNum = static_cast<int64_t>(sequence);
    event.BizIndex = static_cast<int64_t>(sequence);
    event.IsLast = 1;
    return event;
}

sz_hp::MarketObservation make_observation(uint32_t time_ms,
                                          double volume,
                                          double turnover) {
    sz_hp::MarketObservation observation;
    std::strncpy(observation.instrument.data(), "000001.SZ",
                 observation.instrument.size() - 1);
    observation.event_time_ms = time_ms;
    observation.last_price = 10.05;
    observation.total_volume = volume;
    observation.turnover = turnover;
    observation.valid = true;
    for (size_t i = 0; i < 5; ++i) {
        observation.bid_price[i] = 10.0 - static_cast<double>(i) * 0.01;
        observation.ask_price[i] = 10.1 + static_cast<double>(i) * 0.01;
        observation.bid_volume[i] = 100.0;
        observation.ask_volume[i] = 100.0;
    }
    return observation;
}

void print_summary(const char* name, const sz_hp::LatencySummary& summary) {
    std::cout << name
              << " count=" << summary.count
              << " mean_ns=" << std::fixed << std::setprecision(2) << summary.mean_ns
              << " p50_ns=" << summary.p50_ns
              << " p99_ns=" << summary.p99_ns
              << " max_ns=" << summary.max_ns << "\n";
}

bool run_hp_event_benchmark(const Options& options,
                            sz_hp::LatencyRecorder* order_recorder,
                            sz_hp::LatencyRecorder* trade_recorder,
                            volatile uint64_t* sink) {
    sz_hp::SamplerConfig config;
    config.capture_failure_digest = false;
    sz_hp::InstrumentState state("000001.SZ", config);
    const size_t total = options.warmup + options.iterations;
    uint64_t sequence = 1;
    for (size_t i = 0; i < total; ++i) {
        if (i != 0 && i % options.reset_interval == 0) {
            state.reset();
        }
        const LFL2OrderField raw_bid = make_order(sequence++, true);
        const LFL2OrderField raw_ask = make_order(sequence++, false);
        const LFL2TradeField raw_trade = make_trade(
            sequence++, static_cast<uint64_t>(raw_bid.ApplSeqNum),
            static_cast<uint64_t>(raw_ask.ApplSeqNum));
        sz_hp::OrderEvent bid;
        sz_hp::OrderEvent ask;
        sz_hp::TradeEvent trade;
        const bool record = i >= options.warmup;

        uint64_t begin = record ? sz_hp::latency_now_ns() : 0;
        if (!sz_hp::EventAdapter::normalize_order(raw_bid, &bid, 0, i * 3) ||
            !state.process_order(bid)) {
            return false;
        }
        uint64_t end = record ? sz_hp::latency_now_ns() : 0;
        if (record) {
            order_recorder->add(end - begin);
        }

        begin = record ? sz_hp::latency_now_ns() : 0;
        if (!sz_hp::EventAdapter::normalize_order(raw_ask, &ask, 0, i * 3 + 1) ||
            !state.process_order(ask)) {
            return false;
        }
        end = record ? sz_hp::latency_now_ns() : 0;
        if (record) {
            order_recorder->add(end - begin);
        }

        begin = record ? sz_hp::latency_now_ns() : 0;
        if (!sz_hp::EventAdapter::normalize_trade(raw_trade, &trade, 0,
                                                   i * 3 + 2)) {
            return false;
        }
        const sz_hp::SampleDecision decision = state.process_trade(trade);
        end = record ? sz_hp::latency_now_ns() : 0;
        if (!state.available()) {
            return false;
        }
        if (record) {
            trade_recorder->add(end - begin);
        }
        *sink += decision.ready ? 1U : 0U;
    }
    *sink += static_cast<uint64_t>(state.queued_order_count() + state.queued_trade_count());
    return true;
}

template <typename OrderFn, typename TradeFn, typename ResetFn>
bool run_raw_baseline(const Options& options,
                      OrderFn order_fn,
                      TradeFn trade_fn,
                      ResetFn reset_fn,
                      sz_hp::LatencyRecorder* order_recorder,
                      sz_hp::LatencyRecorder* trade_recorder,
                      volatile uint64_t* sink) {
    const size_t total = options.warmup + options.iterations;
    uint64_t sequence = 1;
    for (size_t i = 0; i < total; ++i) {
        if (i != 0 && i % options.reset_interval == 0) {
            reset_fn();
        }
        const LFL2OrderField bid = make_order(sequence++, true);
        const LFL2OrderField ask = make_order(sequence++, false);
        const LFL2TradeField trade = make_trade(
            sequence++, static_cast<uint64_t>(bid.ApplSeqNum),
            static_cast<uint64_t>(ask.ApplSeqNum));
        const bool record = i >= options.warmup;

        uint64_t begin = record ? sz_hp::latency_now_ns() : 0;
        order_fn(&bid);
        uint64_t end = record ? sz_hp::latency_now_ns() : 0;
        if (record) {
            order_recorder->add(end - begin);
        }

        begin = record ? sz_hp::latency_now_ns() : 0;
        order_fn(&ask);
        end = record ? sz_hp::latency_now_ns() : 0;
        if (record) {
            order_recorder->add(end - begin);
        }

        begin = record ? sz_hp::latency_now_ns() : 0;
        trade_fn(&trade);
        end = record ? sz_hp::latency_now_ns() : 0;
        if (record) {
            trade_recorder->add(end - begin);
        }
        *sink += static_cast<uint64_t>(bid.ApplSeqNum & 1);
    }
    return true;
}

bool run_factor_benchmark(const Options& options,
                          sz_hp::LatencyRecorder* factor_recorder,
                          volatile uint64_t* sink) {
    sz_hp::SamplerConfig config;
    config.fee_share = 1000.0;
    sz_hp::InstrumentState state("000001.SZ", config);
    if (!state.book().add_order(1, true, sz_hp::to_price(10.0), 1000, 34200000) ||
        !state.book().add_order(2, false, sz_hp::to_price(10.1), 1000, 34200001)) {
        return false;
    }
    sz_hp::SampleBatch batch;
    batch.valid = true;
    batch.previous_observation.valid = true;
    batch.previous_observation.event_time_ms = 34200000;
    batch.previous_observation.last_price = 10.05;
    batch.previous_observation.total_volume = 1000;
    batch.previous_observation.turnover = 10000;
    batch.current_observation.valid = true;
    batch.current_observation.event_time_ms = 34201000;
    batch.current_observation.last_price = 10.05;
    batch.current_observation.total_volume = 1100;
    batch.current_observation.turnover = 11005;
    for (size_t i = 0; i < 5; ++i) {
        batch.previous_observation.bid_price[i] = 10.0 - static_cast<double>(i) * 0.01;
        batch.previous_observation.ask_price[i] = 10.1 + static_cast<double>(i) * 0.01;
        batch.previous_observation.bid_volume[i] = 100.0 + static_cast<double>(i);
        batch.previous_observation.ask_volume[i] = 100.0 + static_cast<double>(i);
        batch.current_observation.bid_price[i] = batch.previous_observation.bid_price[i];
        batch.current_observation.ask_price[i] = batch.previous_observation.ask_price[i];
        batch.current_observation.bid_volume[i] = batch.previous_observation.bid_volume[i];
        batch.current_observation.ask_volume[i] = batch.previous_observation.ask_volume[i];
    }
    batch.order_flow.buy_order_volume = 100;
    batch.order_flow.sell_order_volume = 100;
    batch.order_flow.raw_trade_pt = 20;
    batch.order_flow.raw_trade_nt = 10;

    for (size_t i = 0; i < options.factor_iterations; ++i) {
        const uint64_t begin = sz_hp::latency_now_ns();
        const sz_hp::FactorInput input = sz_hp::build_factor_input(state, batch);
        const uint64_t end = sz_hp::latency_now_ns();
        if (!input.valid) {
            return false;
        }
        factor_recorder->add(end - begin);
        *sink += static_cast<uint64_t>(input.ordered_values.values[0] != 0.0f);
    }
    return true;
}

bool run_sample_end_to_end_benchmark(const Options& options,
                                     sz_hp::LatencyRecorder* recorder,
                                     volatile uint64_t* sink) {
    sz_hp::SamplerConfig config;
    config.history_amount_threshold = 0.0;
    config.downsample = 1;
    config.minimum_volume_delta = 100.0;
    config.fee_share = 1000.0;
    const size_t total = options.warmup + options.factor_iterations;
    for (size_t i = 0; i < total; ++i) {
        sz_hp::InstrumentState state("000001.SZ", config);
        if (!state.book().add_order(1, true, sz_hp::to_price(10.0), 1000, 34200000) ||
            !state.book().add_order(2, false, sz_hp::to_price(10.1), 1000, 34200001)) {
            return false;
        }
        state.process_observation(make_observation(34200000, 1000.0, 10000.0));

        LFL2OrderField raw_order = make_order(3, true);
        raw_order.Volume = 10.0;
        LFL2TradeField raw_trade = make_trade(4, 3, 2);
        raw_trade.Volume = 10.0;
        sz_hp::OrderEvent order;
        sz_hp::TradeEvent trade;
        if (!sz_hp::EventAdapter::normalize_order(raw_order, &order) ||
            !sz_hp::EventAdapter::normalize_trade(raw_trade, &trade) ||
            !state.process_order(order)) {
            return false;
        }
        state.process_trade(trade);
        const sz_hp::SampleDecision decision =
            state.process_observation(make_observation(34200001, 1100.0, 10100.5));
        if (!decision.ready) {
            return false;
        }

        const bool record = i >= options.warmup;
        const uint64_t begin = record ? sz_hp::latency_now_ns() : 0;
        sz_hp::SampleBatch batch;
        if (!state.consume_sample(&batch)) {
            return false;
        }
        const sz_hp::FactorInput input = sz_hp::build_factor_input(state, batch);
        const uint64_t end = record ? sz_hp::latency_now_ns() : 0;
        if (!input.valid) {
            return false;
        }
        if (record) {
            recorder->add(end - begin);
        }
        *sink += static_cast<uint64_t>(input.cob_values.valid);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        std::cerr << "usage: sz_hp_latency_benchmark [--iterations N] [--warmup N] "
                     "[--reset-interval N] [--factor-iterations N] [--cpu N]\n";
        return 2;
    }
    if (!pin_to_cpu(options.cpu)) {
        std::cerr << "failed to pin benchmark to cpu " << options.cpu << "\n";
        return 2;
    }

    sz_hp::LatencyRecorder order_recorder(true);
    sz_hp::LatencyRecorder trade_recorder(true);
    sz_hp::LatencyRecorder manager_order_recorder(true);
    sz_hp::LatencyRecorder manager_trade_recorder(true);
    sz_hp::LatencyRecorder legacy_order_recorder(true);
    sz_hp::LatencyRecorder legacy_trade_recorder(true);
    sz_hp::LatencyRecorder factor_recorder(true);
    sz_hp::LatencyRecorder sample_end_to_end_recorder(true);
    volatile uint64_t sink = 0;
    if (!run_hp_event_benchmark(options, &order_recorder, &trade_recorder, &sink) ||
        !([&]() {
            ShSzFullOrderBookManager manager;
            return run_raw_baseline(
                options,
                [&manager](const LFL2OrderField* event) { manager.process_order(event); },
                [&manager](const LFL2TradeField* event) { manager.process_trade(event); },
                [&manager]() {
                    ShSzFullOrderBookEngine* engine = manager.get_engine("000001");
                    if (engine != 0) {
                        engine->clear();
                    } else {
                        manager.clear();
                    }
                },
                &manager_order_recorder, &manager_trade_recorder, &sink);
        })() ||
        !([&]() {
            std::unique_ptr<SnapGenerator> generator(new SnapGenerator("000001"));
            return run_raw_baseline(
                options,
                [&generator](const LFL2OrderField* event) { generator->process_order(event); },
                [&generator](const LFL2TradeField* event) { generator->process_trade(event); },
                [&generator]() { generator.reset(new SnapGenerator("000001")); },
                &legacy_order_recorder, &legacy_trade_recorder, &sink);
        })() ||
        !run_factor_benchmark(options, &factor_recorder, &sink) ||
        !run_sample_end_to_end_benchmark(options, &sample_end_to_end_recorder, &sink)) {
        std::cerr << "benchmark fixture failed\n";
        return 1;
    }

    const sz_hp::LatencyReport timer = sz_hp::benchmark_timer_overhead(100000);
    std::cout << "benchmark=sz-hp-candidate-v1"
              << " warmup_cycles=" << options.warmup
              << " measured_cycles=" << options.iterations
              << " reset_interval=" << options.reset_interval
              << " factor_iterations=" << options.factor_iterations
              << " cpu=" << options.cpu
              << " compiler=" << __VERSION__
              << " logging=disabled"
              << " timer_overhead_p50_ns=" << timer.timer_overhead_p50_ns
              << " timer_overhead_max_ns=" << timer.timer_overhead_max_ns
              << " sink=" << sink << "\n";
    print_summary("candidate.order_ingress_to_book_ready", order_recorder.summarize());
    print_summary("candidate.trade_ingress_to_book_ready", trade_recorder.summarize());
    print_summary("current.full_orderbook_manager.order_dispatch",
                  manager_order_recorder.summarize());
    print_summary("current.full_orderbook_manager.trade_dispatch",
                  manager_trade_recorder.summarize());
    print_summary("legacy.snapshot_generator.order_dispatch",
                  legacy_order_recorder.summarize());
    print_summary("legacy.snapshot_generator.trade_dispatch",
                  legacy_trade_recorder.summarize());
    print_summary("candidate.sample_factor", factor_recorder.summarize());
    print_summary("candidate.sample_end_to_end_without_model",
                  sample_end_to_end_recorder.summarize());

    const sz_hp::LatencySummary order = order_recorder.summarize();
    const sz_hp::LatencySummary trade = trade_recorder.summarize();
    std::cout << "hard_gate_5us order=" << (order.p50_ns <= 5000 ? "PASS" : "FAIL")
              << " trade=" << (trade.p50_ns <= 5000 ? "PASS" : "FAIL") << "\n";
    return 0;
}
