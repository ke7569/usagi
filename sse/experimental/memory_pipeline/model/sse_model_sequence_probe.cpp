#include "sse_model_runtime.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: sse_model_sequence_probe SSEMODL1.bin input.f32 output.f32\n";
        return 2;
    }
    sse_model::Model model;
    std::string error;
    if (!model.load(argv[1], &error)) {
        std::cerr << error << '\n';
        return 3;
    }
    std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
    if (!input) return 4;
    const std::streamoff bytes = input.tellg();
    const std::size_t row_bytes = sse_model::kFeatureCount * sizeof(float);
    if (bytes <= 0 || bytes % static_cast<std::streamoff>(row_bytes) != 0) {
        std::cerr << "input size is not an N x 50 Float32 matrix\n";
        return 5;
    }
    input.seekg(0, std::ios::beg);
    std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
    if (!output) return 6;

    sse_model::State state;
    std::array<float, sse_model::kFeatureCount> factors;
    const std::size_t rows = static_cast<std::size_t>(bytes) / row_bytes;
    for (std::size_t row = 0; row < rows; ++row) {
        input.read(reinterpret_cast<char*>(factors.data()),
                   static_cast<std::streamsize>(row_bytes));
        float prediction = 0.0f;
        if (!input || !model.predict(factors, &state, &prediction)) return 7;
        output.write(reinterpret_cast<const char*>(&prediction), sizeof(prediction));
    }
    output.write(reinterpret_cast<const char*>(state.hidden.data()),
                 static_cast<std::streamsize>(state.hidden.size() * sizeof(float)));
    if (!output) return 8;
    std::cout << "rows=" << rows << " output_layout=predictions_then_final_hidden\n";
    return 0;
}
