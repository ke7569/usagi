#include "sze/sampling/mix153060_runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>

namespace {

typedef mix153060::OrderEvent OrderEvent;
typedef mix153060::TradeEvent TradeEvent;
typedef mix153060::EventTiming EventTiming;
typedef mix153060::Sample Sample;
typedef mix153060::SampleBuffer SampleBuffer;
typedef mix153060::Runtime Runtime;

const std::size_t kPriceLevels = 100;
const std::size_t kLevelsPerSide = kPriceLevels / 2;
const std::size_t kWarmupRounds = 20;
const std::size_t kMeasuredRounds = 200;
const std::size_t kTotalRounds = kWarmupRounds + kMeasuredRounds;
const std::int64_t kOrderVolume = 100;
const std::int64_t kBaseTimeStepUs = 250000;

enum class EventKind {
    Order,
    Trade
};

enum class EventPhase {
    Initial,
    Warmup,
    Measured
};

struct Event {
    EventKind kind;
    EventPhase phase;
    OrderEvent order;
    TradeEvent trade;

    Event() : kind(EventKind::Order), phase(EventPhase::Initial), order(), trade() {}
};

struct Scenario {
    std::size_t live_orders;
    std::vector<Event> events;

    Scenario() : live_orders(0), events() {}
};

struct TimingStats {
    std::size_t count;
    std::uint64_t p50;
    std::uint64_t p95;
    std::uint64_t p99;
    std::uint64_t maximum;
    double mean;

    TimingStats()
        : count(0), p50(0), p95(0), p99(0), maximum(0), mean(0.0) {}
};

struct CaseResult {
    std::size_t live_orders;
    std::size_t total_events;
    std::size_t initial_events;
    std::size_t warmup_events;
    std::size_t measured_events;
    std::size_t warmup_sample_events;
    std::size_t measured_sample_events;
    std::size_t measured_samples;
    std::size_t time_trigger_samples;
    std::size_t amount_trigger_samples;
    std::size_t change_trigger_samples;
    std::uint64_t factor_digest;
    std::uint64_t identity_digest;
    std::vector<std::uint64_t> all_total_runtime_ns;
    std::vector<std::uint64_t> all_wall_runtime_ns;
    std::vector<std::uint64_t> sample_work_ns;
    std::vector<std::uint64_t> non_sample_total_runtime_ns;

    explicit CaseResult(std::size_t value)
        : live_orders(value), total_events(0), initial_events(0), warmup_events(0),
          measured_events(0), warmup_sample_events(0), measured_sample_events(0),
          measured_samples(0), time_trigger_samples(0), amount_trigger_samples(0),
          change_trigger_samples(0), factor_digest(14695981039346656037ULL),
          identity_digest(1099511628211ULL), all_total_runtime_ns(),
          all_wall_runtime_ns(), sample_work_ns(), non_sample_total_runtime_ns() {}
};

void require(bool value, const std::string& message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::uint64_t monotonic_raw_ns() {
    timespec value;
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC_RAW) failed");
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

std::uint64_t elapsed_ns(std::uint64_t begin, std::uint64_t end) {
    return end >= begin ? end - begin : 0;
}

double price_for(bool buy, std::size_t level) {
    const int tick = buy ? 950 + static_cast<int>(level)
                         : 1001 + static_cast<int>(level);
    return static_cast<double>(tick) / 100.0;
}

std::int64_t base_time_us(const mix153060::StaticInputs& inputs) {
    std::int64_t value = 0;
    require(mix153060::parse_exchange_time_us("09:31:00.000000",
                                              inputs.trading_date, &value),
            "cannot construct benchmark base time");
    return value;
}

OrderEvent make_order(std::int64_t sequence,
                      std::int64_t time_us,
                      double price,
                      bool buy) {
    OrderEvent value;
    value.app_sequence = sequence;
    value.exchange_time_us = time_us;
    value.local_time_us = time_us;
    value.price = price;
    value.volume = kOrderVolume;
    value.buy = buy;
    value.kind = mix153060::OrderKind::kLimit;
    return value;
}

TradeEvent make_trade(std::int64_t sequence,
                      std::int64_t time_us,
                      std::int64_t buy_id,
                      std::int64_t sell_id) {
    TradeEvent value;
    value.app_sequence = sequence;
    value.exchange_time_us = time_us;
    value.local_time_us = time_us;
    value.price = 10.0;
    value.volume = kOrderVolume;
    value.buy_order_id = buy_id;
    value.sell_order_id = sell_id;
    value.kind = mix153060::TradeKind::kFill;
    return value;
}

void append_order(Scenario* scenario,
                  EventPhase phase,
                  const OrderEvent& order) {
    Event event;
    event.kind = EventKind::Order;
    event.phase = phase;
    event.order = order;
    scenario->events.push_back(event);
}

void append_trade(Scenario* scenario,
                  EventPhase phase,
                  const TradeEvent& trade) {
    Event event;
    event.kind = EventKind::Trade;
    event.phase = phase;
    event.trade = trade;
    scenario->events.push_back(event);
}

Scenario make_scenario(std::size_t live_orders,
                       const mix153060::StaticInputs& inputs) {
    require(live_orders > 0 && (live_orders % 2) == 0,
            "live order count must be a positive even number");
    Scenario scenario;
    scenario.live_orders = live_orders;
    scenario.events.reserve(live_orders + kTotalRounds * 3);
    const std::int64_t base = base_time_us(inputs);
    std::int64_t sequence = 1;

    // The initial book has exactly 50 bid and 50 ask price levels.  Orders
    // are one microsecond apart, so this setup stays below the 100ms trigger.
    for (std::size_t i = 0; i < live_orders; ++i) {
        const bool buy = (i % 2) == 0;
        const std::size_t side_index = i / 2;
        const std::size_t level = side_index % kLevelsPerSide;
        append_order(&scenario, EventPhase::Initial,
                     make_order(sequence++, base + static_cast<std::int64_t>(i + 1),
                                price_for(buy, level), buy));
    }

    for (std::size_t round = 0; round < kTotalRounds; ++round) {
        const EventPhase phase = round < kWarmupRounds
                                     ? EventPhase::Warmup
                                     : EventPhase::Measured;
        const std::int64_t time_us = base + 1000000LL +
            static_cast<std::int64_t>(round) * kBaseTimeStepUs;
        const std::int64_t buy_id = sequence++;
        const std::int64_t sell_id = sequence++;
        const std::int64_t trade_id = sequence++;
        const std::size_t level = round % kLevelsPerSide;
        append_order(&scenario, phase,
                     make_order(buy_id, time_us, price_for(true, level), true));
        append_order(&scenario, phase,
                     make_order(sell_id, time_us + 1, price_for(false, level), false));
        // The trade has the exact pending ask timestamp, so Runtime groups
        // the fill with that order and removes both live orders together.
        append_trade(&scenario, phase, make_trade(trade_id, time_us + 1,
                                                   buy_id, sell_id));
    }
    return scenario;
}

mix153060::StaticInputs benchmark_inputs() {
    mix153060::StaticInputs value;
    value.instrument = "000001";
    value.trading_date = 20260908;
    value.average_amount = 1000000000.0;
    // Each completed round exceeds the amount trigger; the production time
    // trigger is 100 seconds, not 100 milliseconds.
    value.turnover_threshold = 1.0;
    value.free_share = 100000000.0;
    value.pre_close = 10.0;
    value.upper_limit = 11.0;
    value.lower_limit = 9.0;
    value.history_volatility_20d = 0.2;
    return value;
}

void hash_byte(std::uint64_t* digest, unsigned char value) {
    *digest ^= static_cast<std::uint64_t>(value);
    *digest *= 1099511628211ULL;
}

void hash_u64(std::uint64_t* digest, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        hash_byte(digest, static_cast<unsigned char>((value >> (i * 8)) & 0xffU));
    }
}

void hash_u32(std::uint64_t* digest, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        hash_byte(digest, static_cast<unsigned char>((value >> (i * 8)) & 0xffU));
    }
}

void hash_i64(std::uint64_t* digest, std::int64_t value) {
    hash_u64(digest, static_cast<std::uint64_t>(value));
}

void hash_double(std::uint64_t* digest, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hash_u64(digest, bits);
}

void hash_float(std::uint64_t* digest, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hash_u32(digest, bits);
}

void hash_bool(std::uint64_t* digest, bool value) {
    hash_byte(digest, value ? 1U : 0U);
}

void hash_text(std::uint64_t* digest, const std::string& value) {
    hash_u64(digest, static_cast<std::uint64_t>(value.size()));
    for (std::size_t i = 0; i < value.size(); ++i) {
        hash_byte(digest, static_cast<unsigned char>(value[i]));
    }
}

void hash_sample(const Sample& sample,
                 std::uint64_t* factor_digest,
                 std::uint64_t* identity_digest) {
    hash_text(identity_digest, sample.instrument);
    hash_i64(identity_digest, sample.exchange_time_us);
    hash_i64(identity_digest, sample.local_time_us);
    hash_i64(identity_digest, sample.app_sequence);
    hash_i64(identity_digest, sample.cut_index);
    hash_i64(identity_digest, sample.row_in_stock_day);
    hash_i64(identity_digest, sample.window_start_exchange_time_us);
    hash_i64(identity_digest, sample.window_start_app_sequence);
    hash_i64(identity_digest, sample.window_start_cut_index);
    hash_double(identity_digest, sample.last_price);
    hash_double(identity_digest, sample.mid_price);
    hash_double(identity_digest, sample.turnover);
    hash_double(identity_digest, sample.volume);
    hash_bool(identity_digest, sample.amount_trigger);
    hash_bool(identity_digest, sample.time_trigger);
    hash_bool(identity_digest, sample.change_trigger);
    for (std::size_t i = 0; i < mix153060::kFeatureCount; ++i) {
        hash_float(factor_digest, sample.factors[i]);
    }
}

TimingStats stats(const std::vector<std::uint64_t>& values) {
    require(!values.empty(), "cannot summarize an empty timing series");
    std::vector<std::uint64_t> sorted(values);
    std::sort(sorted.begin(), sorted.end());
    long double total = 0.0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        total += static_cast<long double>(values[i]);
    }
    TimingStats result;
    result.count = values.size();
    result.p50 = sorted[(sorted.size() - 1) * 50 / 100];
    result.p95 = sorted[(sorted.size() - 1) * 95 / 100];
    result.p99 = sorted[(sorted.size() - 1) * 99 / 100];
    result.maximum = sorted.back();
    result.mean = static_cast<double>(total / values.size());
    return result;
}

std::string stats_json(const std::vector<std::uint64_t>& values) {
    const TimingStats value = stats(values);
    std::ostringstream output;
    output << std::setprecision(17)
           << "{\"count\":" << value.count
           << ",\"p50_ns\":" << value.p50
           << ",\"p95_ns\":" << value.p95
           << ",\"p99_ns\":" << value.p99
           << ",\"max_ns\":" << value.maximum
           << ",\"mean_ns\":" << value.mean << "}";
    return output.str();
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

void write_sample(std::ostream* output,
                  std::size_t live_orders,
                  std::size_t sample_index,
                  const Sample& sample) {
    if (output == 0) {
        return;
    }
    *output << std::setprecision(17)
            << "{\"live_orders\":" << live_orders
            << ",\"sample_index\":" << sample_index
            << ",\"instrument\":\"" << sample.instrument << "\""
            << ",\"exchange_time_us\":" << sample.exchange_time_us
            << ",\"local_time_us\":" << sample.local_time_us
            << ",\"app_sequence\":" << sample.app_sequence
            << ",\"cut_index\":" << sample.cut_index
            << ",\"row_in_stock_day\":" << sample.row_in_stock_day
            << ",\"window_start_exchange_time_us\":"
            << sample.window_start_exchange_time_us
            << ",\"window_start_app_sequence\":"
            << sample.window_start_app_sequence
            << ",\"window_start_cut_index\":"
            << sample.window_start_cut_index
            << ",\"last_price\":" << sample.last_price
            << ",\"mid_price\":" << sample.mid_price
            << ",\"turnover\":" << sample.turnover
            << ",\"volume\":" << sample.volume
            << ",\"amount_trigger\":" << (sample.amount_trigger ? "true" : "false")
            << ",\"time_trigger\":" << (sample.time_trigger ? "true" : "false")
            << ",\"change_trigger\":" << (sample.change_trigger ? "true" : "false")
            << ",\"factors\":[";
    for (std::size_t i = 0; i < mix153060::kFeatureCount; ++i) {
        if (i != 0) {
            *output << ',';
        }
        *output << std::setprecision(17) << static_cast<double>(sample.factors[i]);
    }
    *output << "]}\n";
    require(output->good(), "sample dump write failed");
}

void record_event_timing(CaseResult* result,
                         const Event& event,
                         const SampleBuffer& buffer,
                         const EventTiming& timing,
                         std::uint64_t wall_runtime_ns,
                         std::ostream* dump) {
    ++result->total_events;
    if (event.phase == EventPhase::Measured) {
        result->all_total_runtime_ns.push_back(timing.total_runtime_ns);
        result->all_wall_runtime_ns.push_back(wall_runtime_ns);
    }
    if (event.phase == EventPhase::Initial) {
        ++result->initial_events;
    } else if (event.phase == EventPhase::Warmup) {
        ++result->warmup_events;
    } else {
        ++result->measured_events;
    }
    if (buffer.count == 0) {
        if (event.phase == EventPhase::Measured)
            result->non_sample_total_runtime_ns.push_back(timing.total_runtime_ns);
        return;
    }
    require(buffer.count == 1, "benchmark event emitted more than one sample");
    if (event.phase == EventPhase::Initial) {
        throw std::runtime_error("initial book unexpectedly emitted a sample");
    }
    if (event.phase == EventPhase::Warmup) {
        ++result->warmup_sample_events;
        return;
    }
    ++result->measured_sample_events;
    ++result->measured_samples;
    result->sample_work_ns.push_back(timing.sample_work_ns);
    const Sample& sample = buffer.values[0];
    require(sample.instrument == "000001", "sample instrument changed");
    require(sample.amount_trigger, "benchmark sample lacks amount trigger");
    ++result->amount_trigger_samples;
    hash_sample(sample, &result->factor_digest, &result->identity_digest);
    write_sample(dump, result->live_orders, result->measured_samples - 1, sample);
}

CaseResult run_case(const Scenario& scenario,
                    const mix153060::StaticInputs& inputs,
                    std::ostream* dump) {
    Runtime runtime(inputs);
    if (!runtime.configured() || !runtime.available()) {
        throw std::runtime_error("runtime unavailable before benchmark events: " +
                                 runtime.failure_reason());
    }
    CaseResult result(scenario.live_orders);
    const std::size_t expected_events = scenario.live_orders + kTotalRounds * 3;
    result.all_total_runtime_ns.reserve(expected_events);
    result.all_wall_runtime_ns.reserve(expected_events);
    result.non_sample_total_runtime_ns.reserve(expected_events - kMeasuredRounds);
    result.sample_work_ns.reserve(kMeasuredRounds);
    SampleBuffer buffer;
    for (std::size_t i = 0; i < scenario.events.size(); ++i) {
        const Event& event = scenario.events[i];
        EventTiming timing;
        const std::uint64_t begin = monotonic_raw_ns();
        if (event.kind == EventKind::Order) {
            runtime.on_order(event.order, &buffer, &timing);
        } else {
            runtime.on_trade(event.trade, &buffer, &timing);
        }
        const std::uint64_t end = monotonic_raw_ns();
        require(runtime.available(), "runtime became unavailable at event " +
                                      std::to_string(i) + ": " +
                                      runtime.failure_reason());
        record_event_timing(&result, event, buffer, timing, elapsed_ns(begin, end), dump);
    }
    require(result.total_events == expected_events, "event count changed");
    require(result.initial_events == scenario.live_orders &&
                result.warmup_events == kWarmupRounds * 3 &&
                result.measured_events == kMeasuredRounds * 3,
            "event phase count changed");
    require(result.warmup_sample_events == kWarmupRounds - 1,
            "warmup sample count changed");
    require(result.measured_sample_events == kMeasuredRounds &&
                result.measured_samples == kMeasuredRounds,
            "measured sample count changed");
    require(result.sample_work_ns.size() == kMeasuredRounds,
            "sample timing count changed");
    require(result.amount_trigger_samples == kMeasuredRounds &&
                result.time_trigger_samples == 0 &&
                result.change_trigger_samples == 0,
            "sample trigger mix changed");
    require(runtime.available(), "runtime unavailable after benchmark events");
    return result;
}

void write_case_header(std::ostream* output, std::size_t live_orders) {
    if (output == 0) {
        return;
    }
    *output << "{\"format\":\"sze_factor_benchmark_v1\",\"live_orders\":"
            << live_orders << ",\"price_levels\":" << kPriceLevels
            << ",\"warmup_rounds\":" << kWarmupRounds
            << ",\"measured_rounds\":" << kMeasuredRounds
            << ",\"factor_count\":" << mix153060::kFeatureCount << "}\n";
    require(output->good(), "sample dump header write failed");
}

std::string result_json(const CaseResult& result) {
    std::ostringstream output;
    output << std::setprecision(17)
           << "{\"benchmark\":\"sze_factor_benchmark_v1\""
           << ",\"live_orders\":" << result.live_orders
           << ",\"price_levels\":" << kPriceLevels
           << ",\"warmup_rounds\":" << kWarmupRounds
           << ",\"measured_rounds\":" << kMeasuredRounds
           << ",\"factor_count\":" << mix153060::kFeatureCount
           << ",\"events\":" << result.total_events
           << ",\"sample_events\":" << result.measured_sample_events
           << ",\"samples\":" << result.measured_samples
           << ",\"warmup_sample_events\":" << result.warmup_sample_events
           << ",\"sample_work_ns\":" << stats_json(result.sample_work_ns)
           << ",\"non_sample_total_runtime_ns\":"
           << stats_json(result.non_sample_total_runtime_ns)
           << ",\"all_event_total_runtime_ns\":"
           << stats_json(result.all_total_runtime_ns)
           << ",\"all_event_wall_runtime_ns\":"
           << stats_json(result.all_wall_runtime_ns)
           << ",\"factor_digest\":\"" << hex_u64(result.factor_digest) << "\""
           << ",\"identity_digest\":\"" << hex_u64(result.identity_digest) << "\""
           << ",\"timing_source\":\"Runtime::EventTiming plus CLOCK_MONOTONIC_RAW wall\""
           << ",\"input_preconstructed\":true"
           << ",\"dump_format\":\"JSONL sample identity and 50 factors\""
           << "}";
    return output.str();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string dump_path;
        if (argc == 3 && std::string(argv[1]) == "--dump") {
            dump_path = argv[2];
            require(!dump_path.empty(), "--dump requires a non-empty path");
        } else if (argc != 1) {
            std::cerr << "usage: sze_factor_benchmark [--dump PATH]\n";
            return 2;
        }

        std::ofstream dump_file;
        std::ostream* dump = 0;
        if (!dump_path.empty()) {
            dump_file.open(dump_path.c_str(), std::ios::out | std::ios::trunc);
            require(dump_file.good(), "cannot open --dump path");
            dump = &dump_file;
        }

        const mix153060::StaticInputs inputs = benchmark_inputs();
        require(inputs.valid(), "benchmark static inputs are invalid");
        const std::size_t cases[] = {1000, 10000, 50000};
        for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            const Scenario scenario = make_scenario(cases[i], inputs);
            write_case_header(dump, cases[i]);
            const CaseResult result = run_case(scenario, inputs, dump);
            std::cout << result_json(result) << '\n';
        }
        if (dump != 0) {
            dump_file.flush();
            require(dump_file.good(), "sample dump flush failed");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sze_factor_benchmark: " << error.what() << '\n';
        return 1;
    }
}
