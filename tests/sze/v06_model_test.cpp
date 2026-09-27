#include "common/model/mix153060/mix153060_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void read_exact(std::istream& in, void* destination, std::size_t size) {
    if (!in.read(static_cast<char*>(destination), size)) throw std::runtime_error("truncated golden");
}
std::uint32_t read_u32(std::istream& in) {
    unsigned char b[4];
    read_exact(in, b, 4);
    return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) |
           (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
}
template<std::size_t N> void read_array(std::istream& in, std::array<float, N>& values) {
    for (std::size_t i = 0; i < N; ++i) {
        const std::uint32_t bits = read_u32(in);
        std::memcpy(&values[i], &bits, 4);
        if (!std::isfinite(values[i])) throw std::runtime_error("non-finite golden");
    }
}
template<std::size_t N> void compare(const std::array<float, N>& actual,
                                    const std::array<float, N>& expected,
                                    float* maximum, float tolerance, const char* stage,
                                    std::size_t row) {
    for (std::size_t i = 0; i < N; ++i) {
        const float error = std::fabs(actual[i] - expected[i]);
        *maximum = std::max(*maximum, error);
        if (!std::isfinite(actual[i]) || error > tolerance) {
            std::cerr << stage << " row=" << row << " index=" << i
                      << " expected=" << expected[i] << " actual=" << actual[i]
                      << " error=" << error << "\n";
            throw std::runtime_error("FP32 stage tolerance exceeded");
        }
    }
}
int agreement(const std::array<float, 4>& values) {
    bool positive = true, negative = true;
    for (std::size_t i = 0; i < 4; ++i) {
        positive = positive && values[i] > 0.0f;
        negative = negative && values[i] < 0.0f;
    }
    return positive ? 1 : (negative ? -1 : 0);
}
}

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: v06_model_test MODEL GOLDEN\n"; return 2; }
    try {
        mix153060::Model model;
        std::string error;
        if (!model.load(argv[1], &error)) throw std::runtime_error(error);
        if (model.output_count() != 4 || model.checkpoint_sha256() !=
                "a56a98638e9c2357022dfd7bc4c98ce320ee8ac746540cab86783948f85bfa11")
            throw std::runtime_error("wrong V06 model identity");
        std::ifstream in(argv[2], std::ios::binary);
        char magic[8]; read_exact(in, magic, 8);
        if (std::memcmp(magic, "V06GOLD1", 8) != 0 || read_u32(in) != 1 ||
            read_u32(in) != 12852 || read_u32(in) != 50 || read_u32(in) != 128 || read_u32(in) != 4)
            throw std::runtime_error("wrong full-day V06 golden header");
        mix153060::State state;
        std::array<float, 50> first;
        std::array<float, 4> first_heads;
        float norm_error = 0, projection_error = 0, gru_error = 0, head_error = 0, hidden_error = 0;
        std::size_t agreement_differences = 0, sign_differences = 0;
        for (std::size_t row = 0; row < 12852; ++row) {
            std::array<float, 50> factors, norm;
            std::array<float, 128> projection, gru;
            std::array<float, 4> expected, actual;
            read_array(in, factors); read_array(in, norm); read_array(in, projection);
            read_array(in, gru); read_array(in, expected);
            mix153060::Trace trace;
            if (!model.predict_heads(factors, &state, &actual, &trace))
                throw std::runtime_error("prediction failed");
            if (row == 0) { first = factors; first_heads = actual; }
            compare(trace.layernorm_output, norm, &norm_error, 2e-5f, "layernorm", row);
            compare(trace.projected_input, projection, &projection_error, 2e-5f, "projection", row);
            compare(trace.gru_output, gru, &gru_error, 2e-5f, "gru", row);
            compare(actual, expected, &head_error, 1e-5f, "heads", row);
            agreement_differences += agreement(actual) != agreement(expected);
            for (std::size_t h = 0; h < 4; ++h)
                sign_differences += (actual[h] > 0) != (expected[h] > 0) || (actual[h] < 0) != (expected[h] < 0);
        }
        std::array<float, 256> final_hidden;
        read_array(in, final_hidden);
        compare(state.hidden, final_hidden, &hidden_error, 2e-5f, "final_hidden", 12852);
        if (state.accepted_rows != 12852 || in.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("sequence count or trailing fixture bytes");
        const mix153060::State before = state;
        std::array<float, 50> invalid = first;
        invalid[17] = std::numeric_limits<float>::quiet_NaN();
        std::array<float, 4> rejected;
        if (model.predict_heads(invalid, &state, &rejected) || state.hidden != before.hidden ||
            state.accepted_rows != before.accepted_rows)
            throw std::runtime_error("invalid input changed recurrent state");
        state.reset();
        float primary = 0;
        if (!model.predict(first, &state, &primary) || primary != first_heads[0] || state.accepted_rows != 1)
            throw std::runtime_error("reset or B15 compatibility API failed");
        std::cout << "rows=12852 max_abs_norm=" << norm_error << " max_abs_projection=" << projection_error
                  << " max_abs_gru=" << gru_error << " max_abs_heads=" << head_error
                  << " max_abs_hidden=" << hidden_error << " sign_differences=" << sign_differences
                  << " agreement_differences=" << agreement_differences << "\n";
        if (agreement_differences != 0 || sign_differences != 0)
            throw std::runtime_error("CPU FP32 decision parity mismatch");
    } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
    return 0;
}
