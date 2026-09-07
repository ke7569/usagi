#ifndef SSE_T0_TEST_ARTIFACTS_H
#define SSE_T0_TEST_ARTIFACTS_H

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace sse_test_artifacts {

inline void write_u16(std::ofstream* out, std::uint16_t value) {
    out->write(reinterpret_cast<const char*>(&value), sizeof(value));
}
inline void write_u32(std::ofstream* out, std::uint32_t value) {
    out->write(reinterpret_cast<const char*>(&value), sizeof(value));
}
inline void write_u64(std::ofstream* out, std::uint64_t value) {
    out->write(reinterpret_cast<const char*>(&value), sizeof(value));
}

inline void write_tick_artifact(const std::string& path) {
    const std::size_t sizes[] = {
        128U * 50U, 128U, 512U * 50U, 512U, 256U * 512U, 256U,
        128U * 256U, 128U, 192U * 128U, 192U * 64U, 192U, 192U,
        64U * 128U, 64U, 64U, 64U, 8U * 64U, 8U, 8U, 1U
    };
    const unsigned char factor_hash[32] = {
        0x24, 0xfd, 0x61, 0xf8, 0xc4, 0x98, 0x27, 0x8d,
        0xd6, 0x7d, 0xb7, 0xf1, 0x83, 0xaa, 0x48, 0x46,
        0xae, 0x50, 0x07, 0x81, 0x43, 0xbb, 0xd3, 0x58,
        0x95, 0x82, 0x99, 0xf8, 0x81, 0x7d, 0xb0, 0x89
    };
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out.write("SSEMODL1", 8);
    write_u32(&out, 1U);
    out.write(reinterpret_cast<const char*>(factor_hash), sizeof(factor_hash));
    for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        std::vector<float> values(sizes[i], 0.0f);
        if (i == 19U) values[0] = 1.5f;
        out.write(reinterpret_cast<const char*>(values.data()),
                  static_cast<std::streamsize>(values.size() * sizeof(float)));
    }
}

inline std::size_t product(const std::vector<std::uint32_t>& shape) {
    std::size_t result = 1U;
    for (std::size_t i = 0; i < shape.size(); ++i) result *= shape[i];
    return result;
}

inline void write_snapshot_artifact(const std::string& path,
                                    std::uint32_t feature_count) {
    struct Spec { const char* name; std::vector<std::uint32_t> shape; };
    std::vector<Spec> specs;
    specs.push_back((Spec){"proj.weight", {128U, feature_count}});
    specs.push_back((Spec){"proj.bias", {128U}});
    specs.push_back((Spec){"feature_layers.0.weight", {512U, feature_count}});
    specs.push_back((Spec){"feature_layers.0.bias", {512U}});
    specs.push_back((Spec){"feature_layers.2.weight", {256U, 512U}});
    specs.push_back((Spec){"feature_layers.2.bias", {256U}});
    specs.push_back((Spec){"feature_layers.4.weight", {128U, 256U}});
    specs.push_back((Spec){"feature_layers.4.bias", {128U}});
    specs.push_back((Spec){"gru.weight_ih_l0", {192U, 128U}});
    specs.push_back((Spec){"gru.weight_hh_l0", {192U, 64U}});
    specs.push_back((Spec){"gru.bias_ih_l0", {192U}});
    specs.push_back((Spec){"gru.bias_hh_l0", {192U}});
    specs.push_back((Spec){"res_gru.weight", {64U, 128U}});
    specs.push_back((Spec){"res_gru.bias", {64U}});
    specs.push_back((Spec){"ln.weight", {64U}});
    specs.push_back((Spec){"ln.bias", {64U}});
    specs.push_back((Spec){"prediction_head.0.weight", {8U, 64U}});
    specs.push_back((Spec){"prediction_head.0.bias", {8U}});
    specs.push_back((Spec){"prediction_head.2.weight", {1U, 8U}});
    specs.push_back((Spec){"prediction_head.2.bias", {1U}});
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out.write("SSESGRU1", 8);
    write_u32(&out, 1U); write_u32(&out, feature_count); write_u32(&out, 64U);
    write_u32(&out, static_cast<std::uint32_t>(specs.size())); write_u32(&out, 0U);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const std::string name(specs[i].name);
        write_u16(&out, static_cast<std::uint16_t>(name.size()));
        out.write(name.data(), static_cast<std::streamsize>(name.size()));
        out.put(static_cast<char>(specs[i].shape.size()));
        out.put('\0'); out.put('\0'); out.put('\0');
        for (std::size_t j = 0; j < specs[i].shape.size(); ++j)
            write_u32(&out, specs[i].shape[j]);
        const std::uint64_t bytes = static_cast<std::uint64_t>(product(specs[i].shape)) * sizeof(float);
        write_u64(&out, bytes);
        std::vector<float> zeros(static_cast<std::size_t>(bytes / sizeof(float)), 0.0f);
        out.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(bytes));
    }
}

inline void write_scaler(const std::string& path, std::size_t feature_count) {
    std::ofstream out(path.c_str());
    out << "{\"mean\":[";
    for (std::size_t i = 0; i < feature_count; ++i) out << (i ? ",0" : "0");
    out << "],\"scale\":[";
    for (std::size_t i = 0; i < feature_count; ++i) out << (i ? ",1" : "1");
    out << "]}";
}

}  // namespace sse_test_artifacts

#endif  // SSE_T0_TEST_ARTIFACTS_H
