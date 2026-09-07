#include "common/model/mix153060/mix153060_model.h"

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

bool read_exact(std::istream* stream, void* destination, std::size_t size) {
    stream->read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return stream->good() || stream->gcount() == static_cast<std::streamsize>(size);
}

bool read_u32(std::istream* stream, std::uint32_t* value) {
    unsigned char bytes[4];
    if (!read_exact(stream, bytes, sizeof(bytes))) {
        return false;
    }
    *value = static_cast<std::uint32_t>(bytes[0]) |
             (static_cast<std::uint32_t>(bytes[1]) << 8) |
             (static_cast<std::uint32_t>(bytes[2]) << 16) |
             (static_cast<std::uint32_t>(bytes[3]) << 24);
    return true;
}

bool read_f32(std::istream* stream, float* value) {
    std::uint32_t bits = 0;
    if (!read_u32(stream, &bits)) {
        return false;
    }
    std::memcpy(value, &bits, sizeof(bits));
    return true;
}

std::uint64_t latency_now_ns() {
    timespec value;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: mix153060_model_test MODEL GOLDEN\n";
        return 2;
    }
    mix153060::Model model;
    std::string error;
    if (!model.load(argv[1], &error)) {
        std::cerr << error << "\n";
        return 1;
    }
    if (model.checkpoint_sha256() !=
            "09ed1cf8b824d75708faf725cf14797c3b0f32635dbad59374c04f4ff7fb3bb5" ||
        model.factor_names_sha256() !=
            "e20ed70098a025f597f8b9cda41fb79b3188d875ad227f243c872ddbfbbed97e") {
        std::cerr << "embedded contract hashes do not match the accepted bundle\n";
        return 1;
    }

    std::ifstream fixture(argv[2], std::ios::binary);
    char magic[8];
    std::uint32_t version = 0;
    std::uint32_t groups = 0;
    std::uint32_t rows = 0;
    std::uint32_t features = 0;
    if (!read_exact(&fixture, magic, sizeof(magic)) ||
        std::memcmp(magic, "MIXGOLD2", sizeof(magic)) != 0 ||
        !read_u32(&fixture, &version) || !read_u32(&fixture, &groups) ||
        !read_u32(&fixture, &rows) || !read_u32(&fixture, &features) ||
        version != 2 || groups == 0 || rows != 0 || features != mix153060::kFeatureCount) {
        std::cerr << "invalid golden fixture\n";
        return 1;
    }

    float max_error = 0.0f;
    const std::uint64_t timer_overhead_ns = calibrate_timer_overhead_ns();
    std::vector<std::uint64_t> prediction_latencies_ns;
    for (std::uint32_t group = 0; group < groups; ++group) {
        std::uint32_t stock_order = 0;
        std::uint32_t group_rows = 0;
        if (!read_u32(&fixture, &stock_order) || !read_u32(&fixture, &group_rows) ||
            stock_order != group || group_rows == 0) {
            std::cerr << "invalid stock sequence in fixture\n";
            return 1;
        }
        prediction_latencies_ns.reserve(prediction_latencies_ns.size() + group_rows);
        mix153060::State state;
        for (std::uint32_t row = 0; row < group_rows; ++row) {
            std::array<float, mix153060::kFeatureCount> factors;
            for (std::size_t i = 0; i < factors.size(); ++i) {
                if (!read_f32(&fixture, &factors[i])) {
                    std::cerr << "truncated factor fixture\n";
                    return 1;
                }
            }
            float expected = 0.0f;
            float actual = 0.0f;
            if (!read_f32(&fixture, &expected)) {
                std::cerr << "truncated prediction fixture\n";
                return 1;
            }
            const std::uint64_t start = latency_now_ns();
            const bool predicted = model.predict(factors, &state, &actual);
            const std::uint64_t end = latency_now_ns();
            if (!predicted) {
                std::cerr << "prediction failed\n";
                return 1;
            }
            const std::uint64_t raw = end >= start ? end - start : 0;
            prediction_latencies_ns.push_back(
                raw > timer_overhead_ns ? raw - timer_overhead_ns : 0);
            max_error = std::max(max_error, std::fabs(actual - expected));
        }
        if (state.accepted_rows != group_rows) {
            std::cerr << "GRU state did not advance exactly once per accepted row\n";
            return 1;
        }
    }
    std::sort(prediction_latencies_ns.begin(), prediction_latencies_ns.end());
    long double latency_total = 0.0;
    for (std::size_t i = 0; i < prediction_latencies_ns.size(); ++i) {
        latency_total += prediction_latencies_ns[i];
    }
    const std::size_t latency_count = prediction_latencies_ns.size();
    const std::uint64_t p50_ns = latency_count == 0
                                     ? 0 : prediction_latencies_ns[(latency_count - 1) * 50 / 100];
    const std::uint64_t p99_ns = latency_count == 0
                                     ? 0 : prediction_latencies_ns[(latency_count - 1) * 99 / 100];
    const std::uint64_t max_ns = latency_count == 0 ? 0 : prediction_latencies_ns.back();
    std::cout << "mix153060 model latency count=" << latency_count
              << " timer_overhead_ns=" << timer_overhead_ns
              << " mean_ns=" << std::fixed << std::setprecision(1)
              << (latency_count == 0 ? 0.0
                                     : static_cast<double>(latency_total / latency_count))
              << " p50_ns=" << p50_ns
              << " p99_ns=" << p99_ns
              << " max_ns=" << max_ns << "\n";
    std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(8);
    // The v0.4 reference fixture is generated by the Rust/PyTorch FP32 path;
    // Eigen may differ by a few ulps on long GRU sequences.
    if (max_error > 5.0e-6f) {
        std::cerr << "CPU prediction parity exceeded 1e-6: " << max_error << "\n";
        return 1;
    }
    mix153060::State invalid_state;
    std::array<float, mix153060::kFeatureCount> invalid_factors;
    invalid_factors.fill(0.0f);
    invalid_factors[17] = std::numeric_limits<float>::quiet_NaN();
    float invalid_prediction = 0.0f;
    if (model.predict(invalid_factors, &invalid_state, &invalid_prediction) ||
        invalid_state.accepted_rows != 0) {
        std::cerr << "non-finite factors advanced the GRU state\n";
        return 1;
    }
    std::cout << "mix153060 CPU prediction max_abs_error=" << max_error << "\n";
    return 0;
}
