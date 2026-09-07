#include "tests/sze/mix153060_golden_fixture.h"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <iostream>

#ifndef SZE_MODEL_KERNEL_LABEL
#define SZE_MODEL_KERNEL_LABEL "unspecified"
#endif

namespace {
std::uint64_t now_ns() {
    timespec value;
    mix153060_test::check(clock_gettime(CLOCK_MONOTONIC_RAW, &value) == 0, "clock_gettime failed");
    return std::uint64_t(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}
}

int main(int argc, char** argv) {
    using namespace mix153060_test;
    try {
        check(argc == 3 || argc == 4, "usage: mix153060_model_benchmark MODEL GOLDEN [REPETITIONS]");
        std::size_t parsed = 0;
        const std::string repeat_text = argc == 4 ? argv[3] : "3";
        const unsigned long repetitions = std::stoul(repeat_text, &parsed);
        check(parsed == repeat_text.size() && repetitions > 0 && repetitions <= 100, "invalid repetitions");
        const Fixture fixture = load_fixture(argv[2]);
        mix153060::Model model;
        std::string error;
        if (!model.load(argv[1], &error)) throw std::runtime_error(error);
        std::size_t rows = 0;
        for (const Group& group : fixture) rows += group.size();
        std::vector<std::uint64_t> latencies(rows);
        std::vector<float> predictions(rows);
        for (unsigned long repetition = 0; repetition < repetitions; ++repetition) {
            mix153060::State warmup;
            for (std::size_t row = 0; row < std::min<std::size_t>(32, fixture[0].size()); ++row) {
                float prediction = 0;
                check(model.predict(fixture[0][row].factors, &warmup, &prediction), "warmup prediction failed");
            }
            std::size_t index = 0;
            std::uint64_t accepted = 0;
            for (const Group& group : fixture) {
                mix153060::State state;
                for (const Row& row : group) {
                    const std::uint64_t start = now_ns();
                    const bool ok = model.predict(row.factors, &state, &predictions[index]);
                    const std::uint64_t end = now_ns();
                    check(ok && end >= start, "timed prediction or clock failed");
                    latencies[index++] = end - start;
                }
                accepted += state.accepted_rows;
            }
            check(index == rows && accepted == rows, "benchmark row count mismatch");
            float max_error = 0;
            index = 0;
            for (const Group& group : fixture)
                for (const Row& row : group)
                    max_error = std::max(max_error, prediction_error(predictions[index++], row.expected));
            std::sort(latencies.begin(), latencies.end());
            std::cout << std::setprecision(9) << "{\"backend\":\"" << SZE_MODEL_KERNEL_LABEL
                      << "\",\"avx2\":"
#if defined(__AVX2__)
                      << "true"
#else
                      << "false"
#endif
                      << ",\"repetition\":" << repetition << ",\"rows\":" << rows
                      << ",\"accepted_rows\":" << accepted
                      << ",\"p50_ns\":" << latencies[(rows - 1) * 50 / 100]
                      << ",\"p95_ns\":" << latencies[(rows - 1) * 95 / 100]
                      << ",\"p99_ns\":" << latencies[(rows - 1) * 99 / 100]
                      << ",\"max_ns\":" << latencies.back()
                      << ",\"max_abs_error\":" << max_error
                      << ",\"clock_overhead_subtracted\":false}\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
