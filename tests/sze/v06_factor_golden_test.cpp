#include "sze/sampling/mix153060_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
template <typename T> T read(std::istream& in) {
    T value;
    if (!in.read(reinterpret_cast<char*>(&value), sizeof(value)))
        throw std::runtime_error("truncated V06 fixture");
    return value;
}
void collect(const mix153060::SampleBuffer& out, std::vector<mix153060::Sample>& rows) {
    for (std::size_t i = 0; i < out.count; ++i) rows.push_back(out.values[i]);
}
mix153060::StaticInputs synthetic_inputs(bool v06) {
    mix153060::StaticInputs inputs;
    inputs.instrument = "000807.SZ"; inputs.trading_date = 20260401;
    inputs.average_amount = 8000; inputs.turnover_threshold = 1;
    inputs.free_share = 200000; inputs.pre_close = 10;
    inputs.upper_limit = 11; inputs.lower_limit = 9; inputs.v06_baseline = v06;
    return inputs;
}
void order(mix153060::Runtime& runtime, std::vector<mix153060::Sample>& rows,
           int64_t seq, int64_t time, bool buy, double price, int64_t volume) {
    mix153060::OrderEvent event;
    event.app_sequence = seq; event.exchange_time_us = event.local_time_us = time;
    event.buy = buy; event.price = price; event.volume = volume;
    mix153060::SampleBuffer out; runtime.on_order(event, &out); collect(out, rows);
    if (!runtime.available()) throw std::runtime_error(runtime.failure_reason());
}
void fill(mix153060::Runtime& runtime, std::vector<mix153060::Sample>& rows,
          int64_t seq, int64_t time, int64_t buy_id) {
    mix153060::TradeEvent event;
    event.app_sequence = seq; event.exchange_time_us = event.local_time_us = time;
    event.buy_order_id = buy_id; event.sell_order_id = 2; event.volume = 100; event.price = 10.02;
    mix153060::SampleBuffer out; runtime.on_trade(event, &out); collect(out, rows);
    if (!runtime.available()) throw std::runtime_error(runtime.failure_reason());
}
void contract_tests() {
    if (mix153060::StaticInputs().v06_baseline) throw std::runtime_error("legacy default changed");
    int64_t open = 0, late = 0;
    mix153060::parse_exchange_time_us("09:30:00.000000", 20260401, &open);
    mix153060::parse_exchange_time_us("14:54:19.000000", 20260401, &late);
    for (bool v06 : {false, true}) {
        mix153060::Runtime runtime(synthetic_inputs(v06));
        std::vector<mix153060::Sample> rows;
        order(runtime, rows, 1, open - 1000000, true, 10, 10000);
        order(runtime, rows, 2, open - 1000000, false, 10.02, 10000);
        order(runtime, rows, 3, open + 1000, true, 9.99, 100);
        order(runtime, rows, 4, open + 2000, true, 10.02, 100);
        fill(runtime, rows, 5, open + 2000, 4);
        order(runtime, rows, 6, open + 2100, true, 10.02, 100);
        fill(runtime, rows, 7, open + 2100, 6);
        mix153060::SampleBuffer out; runtime.flush(&out); collect(out, rows);
        if (rows.size() != (v06 ? 2U : 1U)) throw std::runtime_error("V06 sub-millisecond sampling contract failed");
        mix153060::Runtime closing(synthetic_inputs(v06)); rows.clear();
        order(closing, rows, 1, late, true, 10, 10000);
        order(closing, rows, 2, late + 1000000, false, 10.02, 10000);
        order(closing, rows, 3, late + 101000000, true, 9.99, 100);
        closing.flush(&out); collect(out, rows);
        if (rows.size() != (v06 ? 0U : 1U)) throw std::runtime_error("V06 14:56 boundary contract failed");
    }
    auto invalid = synthetic_inputs(true); invalid.free_share = 0;
    if (invalid.valid()) throw std::runtime_error("V06 requires positive free_share");
}
}

int main(int argc, char** argv) {
    try {
        contract_tests();
        if (argc == 1) { std::cout << "V06 sampling contracts passed\n"; return 0; }
        if (argc > 3) throw std::runtime_error("usage: v06_factor_golden_test [FIXTURE [ACTUAL.csv]]");
        const uint16_t endian = 1;
        if (*reinterpret_cast<const uint8_t*>(&endian) != 1)
            throw std::runtime_error("fixture reader requires little endian host");
        std::ifstream in(argv[1], std::ios::binary);
        char magic[8];
        if (!in.read(magic, 8) || std::memcmp(magic, "V06FACT1", 8))
            throw std::runtime_error("bad V06 fixture magic");
        if (read<uint32_t>(in) != 50) throw std::runtime_error("feature count mismatch");
        const uint32_t events = read<uint32_t>(in), expected_count = read<uint32_t>(in);
        mix153060::StaticInputs inputs;
        inputs.instrument = "000807.SZ";
        inputs.trading_date = read<int32_t>(in);
        inputs.average_amount = read<double>(in);
        inputs.turnover_threshold = read<double>(in);
        inputs.free_share = read<double>(in);
        inputs.pre_close = read<double>(in);
        inputs.upper_limit = read<double>(in);
        inputs.lower_limit = read<double>(in);
        inputs.v06_baseline = true;
        mix153060::Runtime runtime(inputs);
        std::vector<mix153060::Sample> actual;
        mix153060::SampleBuffer output;
        for (uint32_t i = 0; i < events; ++i) {
            const uint8_t kind = read<uint8_t>(in);
            const int64_t seq = read<int64_t>(in), ex = read<int64_t>(in), local = read<int64_t>(in);
            const double price = read<double>(in);
            const int64_t volume = read<int64_t>(in);
            if (kind == 1) {
                mix153060::OrderEvent event;
                event.app_sequence = seq; event.exchange_time_us = ex;
                event.local_time_us = local; event.price = price; event.volume = volume;
                event.buy = read<uint8_t>(in) != 0;
                const uint8_t order_kind = read<uint8_t>(in);
                event.kind = order_kind == 1 ? mix153060::OrderKind::kMarket :
                    order_kind == 2 ? mix153060::OrderKind::kSelfBest : mix153060::OrderKind::kLimit;
                runtime.on_order(event, &output);
            } else if (kind == 2) {
                mix153060::TradeEvent event;
                event.app_sequence = seq; event.exchange_time_us = ex;
                event.local_time_us = local; event.price = price; event.volume = volume;
                event.buy_order_id = read<int64_t>(in); event.sell_order_id = read<int64_t>(in);
                event.kind = read<uint8_t>(in) ? mix153060::TradeKind::kFill : mix153060::TradeKind::kCancel;
                runtime.on_trade(event, &output);
            } else throw std::runtime_error("bad fixture event type");
            if (!runtime.available()) {
                std::cerr << "rejected event=" << i << " AppSeq=" << seq << " reason=" << runtime.failure_reason() << '\n';
                return 1;
            }
            collect(output, actual);
        }
        runtime.flush(&output); collect(output, actual);
        if (argc == 3) {
            std::ofstream dump(argv[2]);
            dump << std::setprecision(10) << "app_seq,ex_time_micros,cut_index,window_start_app_seq,window_start_cut_index,window_start_ex_time_micros,amount,time,change";
            for (int f = 0; f < 50; ++f) dump << ",f" << f;
            dump << '\n';
            for (const auto& row : actual) {
                dump << row.app_sequence << ',' << row.exchange_time_us << ',' << row.cut_index << ','
                     << row.window_start_app_sequence << ',' << row.window_start_cut_index << ','
                     << row.window_start_exchange_time_us << ',' << row.amount_trigger << ',' << row.time_trigger << ',' << row.change_trigger;
                for (float value : row.factors) dump << ',' << value;
                dump << '\n';
            }
        }
        std::size_t identity_errors = 0, factor_errors = 0, reason_errors = 0;
        double max_abs = 0.0;
        for (uint32_t i = 0; i < expected_count; ++i) {
            int64_t key[6]; for (int k = 0; k < 6; ++k) key[k] = read<int64_t>(in);
            uint8_t reason[3]; for (int k = 0; k < 3; ++k) reason[k] = read<uint8_t>(in);
            float factors[50]; for (int f = 0; f < 50; ++f) factors[f] = read<float>(in);
            if (i >= actual.size()) { ++identity_errors; continue; }
            const auto& row = actual[i];
            const int64_t observed[] = {row.exchange_time_us, row.app_sequence, row.cut_index,
                row.window_start_exchange_time_us, row.window_start_app_sequence, row.window_start_cut_index};
            if (!std::equal(key, key + 6, observed)) {
                if (identity_errors < 5) std::cerr << "identity row=" << i << " expected_seq=" << key[1]
                    << " actual_seq=" << row.app_sequence << " expected_cut=" << key[2] << " actual_cut=" << row.cut_index << '\n';
                ++identity_errors;
            }
            if (row.amount_trigger != bool(reason[0]) || row.time_trigger != bool(reason[1]) || row.change_trigger != bool(reason[2])) ++reason_errors;
            for (int f = 0; f < 50; ++f) {
                const double error = std::fabs(double(row.factors[f]) - factors[f]);
                max_abs = std::max(max_abs, error);
                // Tight factor comparison, independent of the looser FP32 model tolerance.
                if (!std::isfinite(row.factors[f]) || !std::isfinite(factors[f]) || error > 2e-6 + 2e-6 * std::fabs(factors[f])) {
                    if (factor_errors < 10) std::cerr << "factor row=" << i << " f=" << f << " expected=" << factors[f] << " actual=" << row.factors[f] << '\n';
                    ++factor_errors;
                }
            }
        }
        // Research replay/mod.rs queues each sample until its 60s-10us label
        // matures (labels.rs::fill_due_pending). clock.rs caps active time at
        // 14:56, so samples after 14:55:00.000010 never enter features.arrow.
        // They are valid causal live samples: keep them in Runtime and the dump,
        // and validate the precisely bounded unlabelled suffix independently.
        int64_t label_cutoff = 0, sampling_end = 0;
        mix153060::parse_exchange_time_us("14:55:00.000010", inputs.trading_date, &label_cutoff);
        mix153060::parse_exchange_time_us("14:56:00.000000", inputs.trading_date, &sampling_end);
        std::size_t invalid_tail = 0;
        for (std::size_t i = expected_count; i < actual.size(); ++i) {
            const auto& row = actual[i];
            bool valid = row.exchange_time_us > label_cutoff && row.exchange_time_us < sampling_end &&
                row.cut_index > row.window_start_cut_index &&
                row.exchange_time_us > row.window_start_exchange_time_us &&
                (row.amount_trigger || row.time_trigger || row.change_trigger);
            if (i > 0) {
                const auto& previous = actual[i - 1];
                const bool amount = row.turnover - previous.turnover >= inputs.turnover_threshold;
                const bool time = row.exchange_time_us - previous.exchange_time_us >= 100000000LL;
                const bool change = std::fabs(row.mid_price - previous.mid_price) > 1e-6 &&
                    row.volume - previous.volume >= 100;
                valid = valid && row.app_sequence > previous.app_sequence &&
                    row.cut_index > previous.cut_index &&
                    row.window_start_app_sequence == previous.app_sequence &&
                    row.window_start_cut_index == previous.cut_index &&
                    row.amount_trigger == amount && row.time_trigger == time && row.change_trigger == change;
            }
            for (float value : row.factors) valid = valid && std::isfinite(value);
            if (!valid) ++invalid_tail;
        }
        if (in.peek() != std::char_traits<char>::eof()) throw std::runtime_error("trailing fixture bytes");
        std::cout << "events=" << events << " actual_samples=" << actual.size() << " expected_samples=" << expected_count
                  << " identity_errors=" << identity_errors << " reason_errors=" << reason_errors
                  << " factor_errors=" << factor_errors << " max_abs=" << std::setprecision(10) << max_abs
                  << " unlabelled_tail=" << (actual.size() > expected_count ? actual.size() - expected_count : 0)
                  << " invalid_tail=" << invalid_tail << '\n';
        return actual.size() < expected_count || identity_errors || reason_errors || factor_errors || invalid_tail ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
