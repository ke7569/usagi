#ifndef USAGI_TEST_MIX153060_GOLDEN_FIXTURE_H
#define USAGI_TEST_MIX153060_GOLDEN_FIXTURE_H

#include "common/model/mix153060/mix153060_model.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mix153060_test {

inline void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Row {
    std::array<float, mix153060::kFeatureCount> factors;
    float expected;
};
typedef std::vector<Row> Group;
typedef std::vector<Group> Fixture;

inline std::uint32_t read_u32(std::istream& input) {
    unsigned char bytes[4];
    check(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes), 4)), "truncated golden fixture");
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

inline float read_float(std::istream& input) {
    const std::uint32_t bits = read_u32(input);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    check(std::isfinite(value), "non-finite golden value");
    return value;
}

inline Fixture load_fixture(const char* path) {
    std::ifstream input(path, std::ios::binary);
    char magic[8];
    check(static_cast<bool>(input.read(magic, 8)) && !std::memcmp(magic, "MIXGOLD2", 8),
          "invalid golden fixture magic");
    const std::uint32_t version = read_u32(input);
    const std::uint32_t groups = read_u32(input);
    const std::uint32_t rows = read_u32(input);
    const std::uint32_t features = read_u32(input);
    check(version == 2 && groups > 0 && rows == 0 && features == mix153060::kFeatureCount,
          "invalid golden fixture header");
    Fixture result;
    for (std::uint32_t group = 0; group < groups; ++group) {
        const std::uint32_t index = read_u32(input);
        const std::uint32_t count = read_u32(input);
        check(index == group && count > 0, "invalid golden group");
        Group values;
        for (std::uint32_t row = 0; row < count; ++row) {
            Row value;
            for (float& factor : value.factors) factor = read_float(input);
            value.expected = read_float(input);
            values.push_back(value);
        }
        result.push_back(std::move(values));
    }
    check(input.peek() == std::char_traits<char>::eof(), "trailing golden fixture data");
    return result;
}

inline float prediction_error(float actual, float expected) {
    check(std::isfinite(actual), "non-finite prediction");
    const float error = std::fabs(actual - expected);
    check(error <= 5.0e-6f, "prediction differs from independent golden reference by more than 5e-6");
    return error;
}

}  // namespace mix153060_test
#endif
