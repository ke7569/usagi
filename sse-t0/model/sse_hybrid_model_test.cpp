#include "sse_hybrid_model.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

void write_u32(std::ofstream* out, std::uint32_t value) {
    out->write(reinterpret_cast<const char*>(&value), sizeof(value));
}
void write_u64(std::ofstream* out, std::uint64_t value) {
    out->write(reinterpret_cast<const char*>(&value), sizeof(value));
}
void write_u16(std::ofstream* out, std::uint16_t value) {
    out->write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_tick_artifact(const std::string& path) {
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

std::size_t product(const std::vector<std::uint32_t>& shape) {
    std::size_t result = 1U;
    for (std::size_t i = 0; i < shape.size(); ++i) result *= shape[i];
    return result;
}

void write_snapshot_artifact(const std::string& path, std::uint32_t feature_count) {
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
        for (std::size_t j = 0; j < specs[i].shape.size(); ++j) write_u32(&out, specs[i].shape[j]);
        const std::uint64_t bytes = static_cast<std::uint64_t>(product(specs[i].shape)) * sizeof(float);
        write_u64(&out, bytes);
        std::vector<float> zeros(static_cast<std::size_t>(bytes / sizeof(float)), 0.0f);
        out.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(bytes));
    }
}

void write_scaler(const std::string& path, std::size_t feature_count) {
    std::ofstream out(path.c_str());
    out << "{\"mean\":[";
    for (std::size_t i = 0; i < feature_count; ++i) out << (i ? ",0" : "0");
    out << "],\"scale\":[";
    for (std::size_t i = 0; i < feature_count; ++i) out << (i ? ",1" : "1");
    out << "]}";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string prefix = std::string("/tmp/sse_hybrid_model_test_") +
                               std::to_string(static_cast<long long>(getpid()));
    const std::string tick = prefix + ".ssemodl1";
    const std::string base = prefix + ".base.ssegru";
    const std::string auc = prefix + ".auc.ssegru";
    const std::string base_scaler = prefix + ".base.json";
    const std::string auc_scaler = prefix + ".auc.json";
    write_tick_artifact(tick);
    write_snapshot_artifact(base, 36U);
    write_snapshot_artifact(auc, 95U);
    write_scaler(base_scaler, 36U);
    write_scaler(auc_scaler, 95U);

    sse_hybrid_model::Model model;
    std::string error;
    assert(model.load(tick, base, base_scaler, auc, auc_scaler, &error));
    assert(model.loaded());
    assert(sse_hybrid_model::Model::selected_source("sse", 34499999999ULL) ==
           sse_hybrid_model::kSnapshotSource);
    assert(sse_hybrid_model::Model::selected_source("sse", 34500000000ULL) ==
           sse_hybrid_model::kTickSource);
    assert(sse_hybrid_model::Model::selected_source("sze", 34500000000ULL) ==
           sse_hybrid_model::kNoSource);
    assert(sse_hybrid_model::Model::selected_source("sse", 86400000000ULL) ==
           sse_hybrid_model::kNoSource);

    sse_hybrid_model::State state;
    sse_hybrid_model::Prediction output;
    std::array<float, sse_model::kFeatureCount> tick_factors = {};
    std::vector<float> snapshot(36U, 0.0f);
    std::vector<float> enhanced(95U, 0.0f);
    assert(model.on_tick(tick_factors, "sse", 34200000000ULL,
                         &state, &output, &error));
    assert(output.tick_generated && !output.selected &&
           state.tick.accepted_rows == 1U);
    assert(model.on_snapshot(snapshot, enhanced, "sse", 34200000001ULL,
                             &state, &output, &error));
    assert(output.snapshot_generated && output.selected &&
           output.selected_source == sse_hybrid_model::kSnapshotSource &&
           state.snapshot.baseline.accepted_rows == 1U &&
           state.snapshot.auction59.accepted_rows == 1U);
    assert(model.on_tick(tick_factors, "sse", 34500000000ULL,
                         &state, &output, &error));
    assert(output.tick_generated && output.selected &&
           output.selected_source == sse_hybrid_model::kTickSource &&
           output.selected_pred == 1.5f && state.tick.accepted_rows == 2U);
    const std::uint64_t snapshot_rows = state.snapshot.baseline.accepted_rows;
    assert(model.on_snapshot(snapshot, enhanced, "sse", 34500000000ULL,
                             &state, &output, &error));
    assert(output.snapshot_generated && !output.selected &&
           state.snapshot.baseline.accepted_rows == snapshot_rows + 1U);
    assert(!model.on_snapshot(snapshot, enhanced, "sse", 34860000000ULL,
                              &state, &output, &error));
    assert(state.snapshot.baseline.accepted_rows == snapshot_rows + 1U);
    tick_factors[0] = std::numeric_limits<float>::infinity();
    assert(!model.on_tick(tick_factors, "sse", 34500000001ULL,
                          &state, &output, &error));
    assert(state.tick.accepted_rows == 2U);

    // Optional real-artifact smoke:
    // tick.bin baseline.bin baseline.json auction.bin auction.json
    if (argc == 6) {
        sse_hybrid_model::Model actual;
        assert(actual.load(argv[1], argv[2], argv[3], argv[4], argv[5], &error));
        sse_hybrid_model::State actual_state;
        sse_hybrid_model::Prediction actual_output;
        std::array<float, sse_model::kFeatureCount> actual_tick = {};
        std::vector<float> actual_snapshot(36U, 0.0f);
        std::vector<float> actual_enhanced(95U, 0.0f);
        assert(actual.on_tick(actual_tick, "sse", 34200000000ULL,
                              &actual_state, &actual_output, &error));
        assert(actual_output.tick_generated && !actual_output.selected);
        const float opening_tick = actual_output.tick_pred;
        assert(actual.on_snapshot(actual_snapshot, actual_enhanced, "sse",
                                  34200000001ULL, &actual_state,
                                  &actual_output, &error));
        assert(actual_output.selected_source == sse_hybrid_model::kSnapshotSource);
        const float opening_snapshot = actual_output.selected_pred;
        assert(actual.on_tick(actual_tick, "sse", 34500000000ULL,
                              &actual_state, &actual_output, &error));
        assert(actual_output.selected_source == sse_hybrid_model::kTickSource);
        std::cout << std::setprecision(9)
                  << "opening_tick_shadow=" << opening_tick
                  << " opening_snapshot_selected=" << opening_snapshot
                  << " post_switch_tick_selected=" << actual_output.selected_pred
                  << " tick_rows=" << actual_state.tick.accepted_rows << "\n";
    }
    return 0;
}
