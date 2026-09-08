#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: sse_model_sequence_compare old.out new.out\n";
        return 2;
    }
    std::ifstream old_input(argv[1], std::ios::binary | std::ios::ate);
    std::ifstream new_input(argv[2], std::ios::binary | std::ios::ate);
    if (!old_input || !new_input) return 3;
    const std::streamoff old_bytes = old_input.tellg();
    const std::streamoff new_bytes = new_input.tellg();
    if (old_bytes <= 0 || old_bytes != new_bytes ||
        old_bytes % static_cast<std::streamoff>(sizeof(float)) != 0) return 4;
    const std::size_t count = static_cast<std::size_t>(old_bytes) / sizeof(float);
    std::vector<float> old_values(count);
    std::vector<float> new_values(count);
    old_input.seekg(0, std::ios::beg);
    new_input.seekg(0, std::ios::beg);
    old_input.read(reinterpret_cast<char*>(old_values.data()), old_bytes);
    new_input.read(reinterpret_cast<char*>(new_values.data()), new_bytes);
    if (!old_input || !new_input) return 5;

    std::size_t different = 0U;
    std::size_t first = count;
    float first_old = 0.0f;
    float first_new = 0.0f;
    float max_abs = 0.0f;
    float max_rel = 0.0f;
    std::size_t max_abs_index = 0U;
    std::size_t max_rel_index = 0U;
    for (std::size_t i = 0U; i < count; ++i) {
        const float old_value = old_values[i];
        const float new_value = new_values[i];
        const float abs_error = std::fabs(old_value - new_value);
        const float scale = std::fmax(std::fmax(std::fabs(old_value),
                                                std::fabs(new_value)),
                                      std::numeric_limits<float>::min());
        const float rel_error = abs_error / scale;
        if (abs_error != 0.0f) {
            ++different;
            if (first == count) {
                first = i;
                first_old = old_value;
                first_new = new_value;
            }
        }
        if (abs_error > max_abs) {
            max_abs = abs_error;
            max_abs_index = i;
        }
        if (rel_error > max_rel) {
            max_rel = rel_error;
            max_rel_index = i;
        }
    }
    std::cout << "count=" << count
              << " different=" << different
              << " max_abs=" << max_abs
              << " max_abs_index=" << max_abs_index
              << " max_rel=" << max_rel
              << " max_rel_index=" << max_rel_index;
    if (first != count) {
        std::cout << " first_index=" << first
                  << " first_old=" << first_old
                  << " first_new=" << first_new;
    }
    std::cout << '\n';
    return 0;
}
