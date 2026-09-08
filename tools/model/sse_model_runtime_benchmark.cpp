#include "common/model/legacy_midmix/sse_model_runtime.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

volatile float g_checksum = 0.0f;

std::uint64_t ticks_begin() {
    unsigned int low = 0U;
    unsigned int high = 0U;
    __asm__ volatile("lfence\n\trdtsc"
                     : "=a"(low), "=d"(high) :: "memory");
    return (static_cast<std::uint64_t>(high) << 32U) | low;
}

std::uint64_t ticks_end() {
    unsigned int low = 0U;
    unsigned int high = 0U;
    unsigned int aux = 0U;
    __asm__ volatile("rdtscp\n\tmov %%ecx, %2\n\tlfence"
                     : "=a"(low), "=d"(high), "=r"(aux) :: "memory");
    (void)aux;
    return (static_cast<std::uint64_t>(high) << 32U) | low;
}

bool generate_input(const std::string& path, std::size_t rows) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    std::uint32_t random = 0x9e3779b9U;
    for (std::size_t row = 0U; row < rows; ++row) {
        for (std::size_t col = 0U; col < sse_model::kFeatureCount; ++col) {
            random = random * 1664525U + 1013904223U;
            const float unit = static_cast<float>(random >> 8U) *
                               (1.0f / 16777216.0f);
            const float value = unit - 0.5f;
            output.write(reinterpret_cast<const char*>(&value), sizeof(value));
        }
    }
    return static_cast<bool>(output);
}

bool read_input(const std::string& path,
                std::vector<std::array<float, sse_model::kFeatureCount> >* rows) {
    std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
    if (!input) return false;
    const std::streamoff bytes = input.tellg();
    const std::size_t row_bytes = sse_model::kFeatureCount * sizeof(float);
    if (bytes <= 0 || bytes % static_cast<std::streamoff>(row_bytes) != 0) {
        return false;
    }
    rows->resize(static_cast<std::size_t>(bytes) / row_bytes);
    input.seekg(0, std::ios::beg);
    for (std::size_t row = 0U; row < rows->size(); ++row) {
        input.read(reinterpret_cast<char*>((*rows)[row].data()),
                   static_cast<std::streamsize>(row_bytes));
    }
    return static_cast<bool>(input);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--generate") == 0) {
        const std::size_t rows = static_cast<std::size_t>(std::strtoull(argv[3], 0, 10));
        if (rows == 0U || !generate_input(argv[2], rows)) return 2;
        std::cout << "generated_rows=" << rows << "\n";
        return 0;
    }
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: sse_model_runtime_benchmark model.bin input.f32 [repeats]\n";
        std::cerr << "   or: sse_model_runtime_benchmark --generate input.f32 rows\n";
        return 2;
    }

    sse_model::Model model;
    std::string error;
    if (!model.load(argv[1], &error)) {
        std::cerr << error << '\n';
        return 3;
    }
    std::vector<std::array<float, sse_model::kFeatureCount> > rows;
    if (!read_input(argv[2], &rows)) return 4;
    const std::size_t repeats = argc == 4
        ? static_cast<std::size_t>(std::strtoull(argv[3], 0, 10)) : 100U;
    if (rows.empty() || repeats == 0U) return 5;

    for (std::size_t warmup = 0U; warmup < 2U; ++warmup) {
        sse_model::State state;
        for (std::size_t row = 0U; row < rows.size(); ++row) {
            float prediction = 0.0f;
            if (!model.predict(rows[row], &state, &prediction)) return 6;
            g_checksum += prediction;
        }
    }

    const std::uint64_t begin = ticks_begin();
    float checksum = 0.0f;
    std::uint64_t accepted_rows = 0U;
    for (std::size_t repeat = 0U; repeat < repeats; ++repeat) {
        sse_model::State state;
        for (std::size_t row = 0U; row < rows.size(); ++row) {
            float prediction = 0.0f;
            if (!model.predict(rows[row], &state, &prediction)) return 7;
            checksum += prediction;
        }
        accepted_rows += state.accepted_rows;
    }
    const std::uint64_t elapsed = ticks_end() - begin;
    g_checksum = checksum;
    std::cout << "rows=" << rows.size()
              << " repeats=" << repeats
              << " cycles_per_row="
              << (static_cast<double>(elapsed) /
                  static_cast<double>(rows.size() * repeats))
              << " accepted_rows=" << accepted_rows
              << " checksum=" << checksum << '\n';
    return 0;
}
