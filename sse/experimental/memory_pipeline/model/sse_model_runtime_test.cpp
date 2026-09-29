#include "sse_model_runtime.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

int main() {
    const std::string path = "/tmp/sse_model_runtime_test.bin";
    const std::size_t sizes[] = {
        128U * 50U, 128U, 512U * 50U, 512U, 256U * 512U, 256U,
        128U * 256U, 128U, 192U * 128U, 192U * 64U, 192U, 192U,
        64U * 128U, 64U, 64U, 64U, 8U * 64U, 8U, 8U, 1U
    };
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output.write("SSEMODL1", 8);
    const unsigned int version = 1U;
    output.write(reinterpret_cast<const char*>(&version), sizeof(version));
    const unsigned char factor_hash[32] = {
        0x24, 0xfd, 0x61, 0xf8, 0xc4, 0x98, 0x27, 0x8d,
        0xd6, 0x7d, 0xb7, 0xf1, 0x83, 0xaa, 0x48, 0x46,
        0xae, 0x50, 0x07, 0x81, 0x43, 0xbb, 0xd3, 0x58,
        0x95, 0x82, 0x99, 0xf8, 0x81, 0x7d, 0xb0, 0x89
    };
    output.write(reinterpret_cast<const char*>(factor_hash), sizeof(factor_hash));
    for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        std::vector<float> zeros(sizes[i], 0.0f);
        if (i == 19U) zeros[0] = 1.5f;
        output.write(reinterpret_cast<const char*>(zeros.data()),
                     static_cast<std::streamsize>(zeros.size() * sizeof(float)));
    }
    output.close();

    sse_model::Model model;
    std::string error;
    assert(model.load(path, &error));
    assert(error.empty());
    sse_model::State state;
    std::array<float, sse_model::kFeatureCount> factors = {};
    float prediction = 0.0f;
    assert(model.predict(factors, &state, &prediction));
    assert(prediction == 1.5f);
    assert(state.accepted_rows == 1U);
    state.reset();
    assert(state.accepted_rows == 0U);
    std::fstream corrupt(path.c_str(), std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(12, std::ios::beg);
    const unsigned char bad_hash_byte = 0U;
    corrupt.write(reinterpret_cast<const char*>(&bad_hash_byte), 1);
    corrupt.close();
    sse_model::Model rejected;
    error.clear();
    assert(!rejected.load(path, &error));
    assert(error.find("factor contract") != std::string::npos);
    std::remove(path.c_str());
    std::puts("sse_model_runtime_test: PASS");
    return 0;
}
