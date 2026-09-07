#include "sze/sampling/mix153060_runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

bool read_exact(std::istream* input, void* destination, std::size_t size) {
    input->read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return input->good() || input->gcount() == static_cast<std::streamsize>(size);
}

bool read_u8(std::istream* input, std::uint8_t* value) {
    return read_exact(input, value, sizeof(*value));
}

bool read_u32(std::istream* input, std::uint32_t* value) {
    unsigned char bytes[4];
    if (!read_exact(input, bytes, sizeof(bytes))) {
        return false;
    }
    *value = static_cast<std::uint32_t>(bytes[0]) |
             (static_cast<std::uint32_t>(bytes[1]) << 8) |
             (static_cast<std::uint32_t>(bytes[2]) << 16) |
             (static_cast<std::uint32_t>(bytes[3]) << 24);
    return true;
}

bool read_i32(std::istream* input, std::int32_t* value) {
    std::uint32_t raw = 0;
    if (!read_u32(input, &raw)) {
        return false;
    }
    std::memcpy(value, &raw, sizeof(raw));
    return true;
}

bool read_i64(std::istream* input, std::int64_t* value) {
    unsigned char bytes[8];
    if (!read_exact(input, bytes, sizeof(bytes))) {
        return false;
    }
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
        raw |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    std::memcpy(value, &raw, sizeof(raw));
    return true;
}

bool read_f32(std::istream* input, float* value) {
    std::uint32_t raw = 0;
    if (!read_u32(input, &raw)) {
        return false;
    }
    std::memcpy(value, &raw, sizeof(raw));
    return true;
}

bool read_f64(std::istream* input, double* value) {
    std::int64_t raw = 0;
    if (!read_i64(input, &raw)) {
        return false;
    }
    std::memcpy(value, &raw, sizeof(raw));
    return true;
}

bool read_common(std::istream* input,
                 std::int64_t* app_sequence,
                 std::int64_t* exchange_time_us,
                 std::int64_t* local_time_us,
                 double* price,
                 std::int64_t* volume) {
    return read_i64(input, app_sequence) && read_i64(input, exchange_time_us) &&
           read_i64(input, local_time_us) && read_f64(input, price) &&
           read_i64(input, volume);
}

bool read_order(std::istream* input, mix153060::OrderEvent* event) {
    std::uint8_t buy = 0;
    std::uint8_t kind = 0;
    if (!read_common(input, &event->app_sequence, &event->exchange_time_us,
                     &event->local_time_us, &event->price, &event->volume) ||
        !read_u8(input, &buy) || !read_u8(input, &kind)) {
        return false;
    }
    event->buy = buy != 0;
    event->kind = kind == 1 ? mix153060::OrderKind::kMarket
                            : kind == 2 ? mix153060::OrderKind::kSelfBest
                                        : mix153060::OrderKind::kLimit;
    return true;
}

bool read_trade(std::istream* input, mix153060::TradeEvent* event) {
    std::uint8_t kind = 0;
    if (!read_common(input, &event->app_sequence, &event->exchange_time_us,
                     &event->local_time_us, &event->price, &event->volume) ||
        !read_i64(input, &event->buy_order_id) ||
        !read_i64(input, &event->sell_order_id) || !read_u8(input, &kind)) {
        return false;
    }
    event->kind = kind == 1 ? mix153060::TradeKind::kFill
                            : mix153060::TradeKind::kCancel;
    return true;
}

void append_samples(const mix153060::SampleBuffer& buffer,
                    std::vector<mix153060::Sample>* output) {
    for (std::size_t i = 0; i < buffer.count; ++i) {
        output->push_back(buffer.values[i]);
    }
}

struct ExpectedIdentity {
    std::int64_t row;
    std::int64_t exchange_time_us;
    std::int64_t app_sequence;
    std::int64_t cut_index;
    std::int64_t window_start_exchange_time_us;
    std::int64_t window_start_app_sequence;
    std::int64_t window_start_cut_index;
};

bool read_expected_identity(std::istream* input, ExpectedIdentity* value) {
    return read_i64(input, &value->row) &&
           read_i64(input, &value->exchange_time_us) &&
           read_i64(input, &value->app_sequence) &&
           read_i64(input, &value->cut_index) &&
           read_i64(input, &value->window_start_exchange_time_us) &&
           read_i64(input, &value->window_start_app_sequence) &&
           read_i64(input, &value->window_start_cut_index);
}

bool identity_matches(const mix153060::Sample& actual,
                      const ExpectedIdentity& expected) {
    return actual.row_in_stock_day == expected.row &&
           actual.exchange_time_us == expected.exchange_time_us &&
           actual.app_sequence == expected.app_sequence &&
           actual.cut_index == expected.cut_index &&
           actual.window_start_exchange_time_us == expected.window_start_exchange_time_us &&
           actual.window_start_app_sequence == expected.window_start_app_sequence &&
           actual.window_start_cut_index == expected.window_start_cut_index;
}

std::uint64_t latency_now_ns() {
    timespec value;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

std::uint64_t elapsed_ns(std::uint64_t start,
                         std::uint64_t end,
                         std::uint64_t timer_overhead_ns) {
    const std::uint64_t raw = end >= start ? end - start : 0;
    return raw > timer_overhead_ns ? raw - timer_overhead_ns : 0;
}

std::uint64_t calibrate_timer_overhead_ns() {
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i < 10000; ++i) {
        const std::uint64_t start = latency_now_ns();
        const std::uint64_t end = latency_now_ns();
        const std::uint64_t elapsed = end >= start ? end - start : 0;
        best = std::min(best, elapsed);
    }
    return best == std::numeric_limits<std::uint64_t>::max() ? 0 : best;
}

struct LatencyStats {
    double mean_ns;
    std::uint64_t p50_ns;
    std::uint64_t p99_ns;
    std::uint64_t max_ns;

    LatencyStats() : mean_ns(0.0), p50_ns(0), p99_ns(0), max_ns(0) {}
};

LatencyStats latency_stats(std::vector<std::uint64_t> values) {
    LatencyStats result;
    if (values.empty()) {
        return result;
    }
    long double total = 0.0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        total += values[i];
    }
    std::sort(values.begin(), values.end());
    result.mean_ns = static_cast<double>(total / values.size());
    result.p50_ns = values[(values.size() - 1) * 50 / 100];
    result.p99_ns = values[(values.size() - 1) * 99 / 100];
    result.max_ns = values.back();
    return result;
}

void print_latency(const char* event,
                   const std::vector<std::uint64_t>& values,
                   const LatencyStats& stats) {
    std::cout << "mix153060 latency event=" << event
              << " count=" << values.size()
              << " mean_ns=" << std::fixed << std::setprecision(1) << stats.mean_ns
              << " p50_ns=" << stats.p50_ns
              << " p99_ns=" << stats.p99_ns
              << " max_ns=" << stats.max_ns << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool latency_gate = false;
    bool event_timing_enabled = false;
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: mix153060_runtime_test RUNTIME_GOLDEN "
                     "[--latency-gate] [--event-timing]\n";
        return 2;
    }
    for (int arg = 2; arg < argc; ++arg) {
        if (std::strcmp(argv[arg], "--latency-gate") == 0) {
            latency_gate = true;
        } else if (std::strcmp(argv[arg], "--event-timing") == 0) {
            event_timing_enabled = true;
        } else {
            std::cerr << "unknown argument: " << argv[arg] << "\n";
            return 2;
        }
    }
    std::ifstream input(argv[1], std::ios::binary);
    char magic[8];
    std::uint32_t version = 0;
    std::uint32_t groups = 0;
    std::uint32_t factors = 0;
    std::uint32_t rows_per_stock = 0;
    if (!read_exact(&input, magic, sizeof(magic)) || !read_u32(&input, &version) ||
        !read_u32(&input, &groups) || !read_u32(&input, &factors) ||
        !read_u32(&input, &rows_per_stock) ||
        (std::memcmp(magic, "MIXRUN01", sizeof(magic)) != 0 &&
         std::memcmp(magic, "MIXRUN02", sizeof(magic)) != 0) ||
        (version != 1 && version != 2) ||
        groups == 0 || factors != mix153060::kFeatureCount) {
        std::cerr << "invalid runtime fixture header\n";
        return 1;
    }

    double max_error = 0.0;
    std::size_t differing_values = 0;
    std::size_t checked_rows = 0;
    const std::uint64_t timer_overhead_ns = calibrate_timer_overhead_ns();
    std::vector<std::uint64_t> order_latencies_ns;
    std::vector<std::uint64_t> trade_latencies_ns;
    std::vector<std::uint64_t> order_no_sample_latencies_ns;
    std::vector<std::uint64_t> order_sample_latencies_ns;
    std::vector<std::uint64_t> trade_no_sample_latencies_ns;
    std::vector<std::uint64_t> trade_sample_latencies_ns;
    std::vector<std::uint64_t> order_book_mutation_ns;
    std::vector<std::uint64_t> trade_book_mutation_ns;
    std::vector<std::uint64_t> order_sample_work_ns;
    std::vector<std::uint64_t> trade_sample_work_ns;
    std::vector<std::uint64_t> order_total_runtime_ns;
    std::vector<std::uint64_t> trade_total_runtime_ns;
    for (std::uint32_t group = 0; group < groups; ++group) {
        std::uint32_t stock_order = 0;
        char instrument_bytes[16];
        std::int32_t trading_date = 0;
        mix153060::StaticInputs static_inputs;
        std::uint32_t frame_count = 0;
        std::uint32_t expected_count = 0;
        if (!read_u32(&input, &stock_order) ||
            !read_exact(&input, instrument_bytes, sizeof(instrument_bytes)) ||
            !read_i32(&input, &trading_date) ||
            !read_f64(&input, &static_inputs.average_amount) ||
            !read_f64(&input, &static_inputs.turnover_threshold) ||
            (version == 2 && !read_f64(&input, &static_inputs.free_share)) ||
            !read_f64(&input, &static_inputs.pre_close) ||
            !read_f64(&input, &static_inputs.upper_limit) ||
            !read_f64(&input, &static_inputs.lower_limit) ||
            !read_f64(&input, &static_inputs.history_volatility_20d) ||
            !read_u32(&input, &frame_count) || !read_u32(&input, &expected_count)) {
            std::cerr << "truncated group header\n";
            return 1;
        }
        std::size_t instrument_size = 0;
        while (instrument_size < sizeof(instrument_bytes) &&
               instrument_bytes[instrument_size] != '\0') {
            ++instrument_size;
        }
        static_inputs.instrument.assign(instrument_bytes, instrument_size);
        static_inputs.trading_date = trading_date;
        mix153060::Runtime runtime(static_inputs);
        if (!runtime.configured()) {
            std::cerr << "runtime rejected static inputs for stock " << stock_order << "\n";
            return 1;
        }
        std::vector<mix153060::Sample> samples;
        samples.reserve(expected_count);
        order_latencies_ns.reserve(order_latencies_ns.size() + frame_count);
        trade_latencies_ns.reserve(trade_latencies_ns.size() + frame_count);
        mix153060::SampleBuffer buffer;
        for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
            std::uint8_t frame_kind = 0;
            std::uint32_t fill_count = 0;
            if (!read_u8(&input, &frame_kind) || !read_u32(&input, &fill_count)) {
                std::cerr << "truncated frame header\n";
                return 1;
            }
            if (frame_kind == 1) {
                mix153060::OrderEvent order;
                if (!read_order(&input, &order)) {
                    std::cerr << "truncated order frame\n";
                    return 1;
                }
                mix153060::EventTiming event_timing;
                const std::uint64_t start = latency_now_ns();
                runtime.on_order(order, &buffer,
                                 event_timing_enabled ? &event_timing : 0);
                const std::uint64_t end = latency_now_ns();
                const std::uint64_t latency = elapsed_ns(start, end, timer_overhead_ns);
                order_latencies_ns.push_back(latency);
                (buffer.count == 0 ? order_no_sample_latencies_ns
                                   : order_sample_latencies_ns).push_back(latency);
                if (event_timing_enabled) {
                    order_book_mutation_ns.push_back(event_timing.book_mutation_ns);
                    order_sample_work_ns.push_back(event_timing.sample_work_ns);
                    order_total_runtime_ns.push_back(event_timing.total_runtime_ns);
                }
                append_samples(buffer, &samples);
                for (std::uint32_t fill = 0; fill < fill_count; ++fill) {
                    mix153060::TradeEvent trade;
                    if (!read_trade(&input, &trade)) {
                        std::cerr << "truncated grouped fill\n";
                        return 1;
                    }
                    mix153060::EventTiming event_timing;
                    const std::uint64_t start = latency_now_ns();
                    runtime.on_trade(trade, &buffer,
                                     event_timing_enabled ? &event_timing : 0);
                    const std::uint64_t end = latency_now_ns();
                    const std::uint64_t latency = elapsed_ns(start, end, timer_overhead_ns);
                    trade_latencies_ns.push_back(latency);
                    (buffer.count == 0 ? trade_no_sample_latencies_ns
                                       : trade_sample_latencies_ns).push_back(latency);
                    if (event_timing_enabled) {
                        trade_book_mutation_ns.push_back(event_timing.book_mutation_ns);
                        trade_sample_work_ns.push_back(event_timing.sample_work_ns);
                        trade_total_runtime_ns.push_back(event_timing.total_runtime_ns);
                    }
                    append_samples(buffer, &samples);
                }
            } else if (frame_kind == 2 && fill_count == 0) {
                mix153060::TradeEvent trade;
                if (!read_trade(&input, &trade)) {
                    std::cerr << "truncated trade frame\n";
                    return 1;
                }
                mix153060::EventTiming event_timing;
                const std::uint64_t start = latency_now_ns();
                runtime.on_trade(trade, &buffer,
                                 event_timing_enabled ? &event_timing : 0);
                const std::uint64_t end = latency_now_ns();
                const std::uint64_t latency = elapsed_ns(start, end, timer_overhead_ns);
                trade_latencies_ns.push_back(latency);
                (buffer.count == 0 ? trade_no_sample_latencies_ns
                                   : trade_sample_latencies_ns).push_back(latency);
                if (event_timing_enabled) {
                    trade_book_mutation_ns.push_back(event_timing.book_mutation_ns);
                    trade_sample_work_ns.push_back(event_timing.sample_work_ns);
                    trade_total_runtime_ns.push_back(event_timing.total_runtime_ns);
                }
                append_samples(buffer, &samples);
            } else {
                std::cerr << "invalid frame kind/count\n";
                return 1;
            }
        }
        runtime.flush(&buffer);
        append_samples(buffer, &samples);
        if (samples.size() != expected_count) {
            std::cerr << "stock " << stock_order << " sample count actual=" << samples.size()
                      << " expected=" << expected_count << " frames=" << frame_count << "\n";
            return 1;
        }
        for (std::uint32_t row = 0; row < expected_count; ++row) {
            ExpectedIdentity expected_identity;
            if (!read_expected_identity(&input, &expected_identity)) {
                std::cerr << "truncated expected identity\n";
                return 1;
            }
            if (!identity_matches(samples[row], expected_identity)) {
                std::cerr << "stock " << stock_order << " row " << row
                          << " identity mismatch actual=(" << samples[row].row_in_stock_day
                          << ',' << samples[row].exchange_time_us << ',' << samples[row].app_sequence
                          << ',' << samples[row].cut_index << ','
                          << samples[row].window_start_exchange_time_us << ','
                          << samples[row].window_start_app_sequence << ','
                          << samples[row].window_start_cut_index << ") expected=("
                          << expected_identity.row << ',' << expected_identity.exchange_time_us << ','
                          << expected_identity.app_sequence << ',' << expected_identity.cut_index << ','
                          << expected_identity.window_start_exchange_time_us << ','
                          << expected_identity.window_start_app_sequence << ','
                          << expected_identity.window_start_cut_index << ")\n";
                return 1;
            }
            for (std::size_t factor = 0; factor < mix153060::kFeatureCount; ++factor) {
                float expected = 0.0f;
                if (!read_f32(&input, &expected)) {
                    std::cerr << "truncated expected factors\n";
                    return 1;
                }
                const double error = std::fabs(static_cast<double>(samples[row].factors[factor]) -
                                               static_cast<double>(expected));
                max_error = std::max(max_error, error);
                if (error != 0.0) {
                    ++differing_values;
                    if (differing_values <= 10) {
                        std::cerr << "stock " << stock_order << " row " << row
                                  << " factor " << factor << " actual="
                                  << samples[row].factors[factor] << " expected=" << expected
                                  << " error=" << error << "\n";
                    }
                }
            }
            ++checked_rows;
        }
    }
    std::cout << "mix153060 runtime rows=" << checked_rows
              << " rows_per_stock=" << rows_per_stock
              << " differing_values=" << differing_values
              << " max_abs_error=" << max_error << "\n";
    const LatencyStats order_stats = latency_stats(order_latencies_ns);
    const LatencyStats trade_stats = latency_stats(trade_latencies_ns);
    const LatencyStats order_no_sample_stats = latency_stats(order_no_sample_latencies_ns);
    const LatencyStats order_sample_stats = latency_stats(order_sample_latencies_ns);
    const LatencyStats trade_no_sample_stats = latency_stats(trade_no_sample_latencies_ns);
    const LatencyStats trade_sample_stats = latency_stats(trade_sample_latencies_ns);
    std::cout << "mix153060 latency timer_overhead_ns=" << timer_overhead_ns << "\n";
    print_latency("order", order_latencies_ns, order_stats);
    print_latency("trade", trade_latencies_ns, trade_stats);
    print_latency("order_no_sample", order_no_sample_latencies_ns, order_no_sample_stats);
    print_latency("order_sample", order_sample_latencies_ns, order_sample_stats);
    print_latency("trade_no_sample", trade_no_sample_latencies_ns, trade_no_sample_stats);
    print_latency("trade_sample", trade_sample_latencies_ns, trade_sample_stats);
    if (event_timing_enabled) {
        print_latency("order_book_mutation", order_book_mutation_ns,
                      latency_stats(order_book_mutation_ns));
        print_latency("trade_book_mutation", trade_book_mutation_ns,
                      latency_stats(trade_book_mutation_ns));
        print_latency("order_sample_work", order_sample_work_ns,
                      latency_stats(order_sample_work_ns));
        print_latency("trade_sample_work", trade_sample_work_ns,
                      latency_stats(trade_sample_work_ns));
        print_latency("order_total_runtime", order_total_runtime_ns,
                      latency_stats(order_total_runtime_ns));
        print_latency("trade_total_runtime", trade_total_runtime_ns,
                      latency_stats(trade_total_runtime_ns));
    }
    if (differing_values != 0) {
        return 1;
    }
    if (latency_gate && (order_latencies_ns.empty() || trade_latencies_ns.empty() ||
                         order_stats.p50_ns >= 5000 || trade_stats.p50_ns >= 5000)) {
        std::cerr << "mix153060 5us p50 latency gate failed\n";
        return 1;
    }
    return 0;
}
