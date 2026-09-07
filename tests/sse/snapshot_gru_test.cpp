#include "sse/model/snapshot_ensemble.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

void write_u16(std::ofstream* out, std::uint16_t value) { out->write(reinterpret_cast<const char*>(&value), sizeof(value)); }
void write_u32(std::ofstream* out, std::uint32_t value) { out->write(reinterpret_cast<const char*>(&value), sizeof(value)); }
void write_u64(std::ofstream* out, std::uint64_t value) { out->write(reinterpret_cast<const char*>(&value), sizeof(value)); }

std::size_t count(const std::vector<std::uint32_t>& shape) {
    std::size_t result = 1U;
    for (std::size_t i = 0; i < shape.size(); ++i) result *= shape[i];
    return result;
}

void write_zero_artifact(const std::string& path, std::uint32_t feature_count) {
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
    std::ofstream out(path.c_str(), std::ios::binary);
    const char magic[8] = {'S','S','E','S','G','R','U','1'};
    out.write(magic, sizeof(magic));
    write_u32(&out, 1U); write_u32(&out, feature_count); write_u32(&out, 64U);
    write_u32(&out, static_cast<std::uint32_t>(specs.size())); write_u32(&out, 0U);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const std::string name(specs[i].name);
        write_u16(&out, static_cast<std::uint16_t>(name.size()));
        out.write(name.data(), static_cast<std::streamsize>(name.size()));
        out.put(static_cast<char>(specs[i].shape.size()));
        out.put('\0'); out.put('\0'); out.put('\0');
        for (std::size_t j = 0; j < specs[i].shape.size(); ++j) write_u32(&out, specs[i].shape[j]);
        const std::uint64_t bytes = static_cast<std::uint64_t>(count(specs[i].shape)) * sizeof(float);
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
    using sse_snapshot_gru::DualState;
    using sse_snapshot_gru::Ensemble;
    using sse_snapshot_gru::Prediction;

    float wb = 0.0f, wa = 0.0f;
    assert(!Ensemble::route(34199999999ULL, &wb, &wa));
    assert(Ensemble::route(34200000000ULL, &wb, &wa) && wb == 0.25f && wa == 0.75f);
    assert(Ensemble::route(34259999999ULL, &wb, &wa) && wb == 0.25f && wa == 0.75f);
    assert(Ensemble::route(34260000000ULL, &wb, &wa) && wb == 0.50f && wa == 0.50f);
    assert(Ensemble::route(34439999999ULL, &wb, &wa) && wb == 0.50f && wa == 0.50f);
    assert(Ensemble::route(34440000000ULL, &wb, &wa) && wb == 0.75f && wa == 0.25f);
    assert(Ensemble::route(34499999999ULL, &wb, &wa) && wb == 0.75f && wa == 0.25f);
    assert(Ensemble::route(34500000000ULL, &wb, &wa) && wb == 0.75f && wa == 0.25f);
    assert(Ensemble::route(34859999999ULL, &wb, &wa) && wb == 0.75f && wa == 0.25f);
    assert(!Ensemble::route(34860000000ULL, &wb, &wa));

    const std::string prefix = std::string("/tmp/sse_snapshot_gru_test_") +
                               std::to_string(static_cast<long long>(getpid()));
    const std::string base_bin = prefix + "_base.bin";
    const std::string auc_bin = prefix + "_auc.bin";
    const std::string base_json = prefix + "_base.json";
    const std::string auc_json = prefix + "_auc.json";
    write_zero_artifact(base_bin, 36U);
    write_zero_artifact(auc_bin, 95U);
    write_scaler(base_json, 36U);
    write_scaler(auc_json, 95U);

    Ensemble ensemble;
    std::string error;
    assert(ensemble.load(base_bin, base_json, auc_bin, auc_json, &error));
    DualState state;
    Prediction prediction;
    std::vector<float> snapshot(36U, 0.0f);
    std::vector<float> enhanced(95U, 0.0f);
    assert(ensemble.predict(snapshot, enhanced, "sse", 34200000000ULL,
                            &state, &prediction, &error));
    assert(prediction.valid && prediction.ensemble_pred == 0.0f);
    assert(state.baseline.accepted_rows == 1U && state.auction59.accepted_rows == 1U);
    const std::uint64_t rows_before = state.baseline.accepted_rows;
    enhanced[80] = std::numeric_limits<float>::infinity();
    assert(!ensemble.predict(snapshot, enhanced, "sse", 34200000001ULL,
                             &state, &prediction, &error));
    assert(state.baseline.accepted_rows == rows_before && state.auction59.accepted_rows == rows_before);
    enhanced[80] = 0.0f;
    assert(!ensemble.predict(snapshot, enhanced, "SZE", 34200000001ULL,
                             &state, &prediction, &error));
    assert(!ensemble.predict(snapshot, enhanced, "sse", 34860000000ULL,
                             &state, &prediction, &error));

    // Optional artifact smoke: pass baseline.bin baseline.json auction.bin
    // auction.json to exercise the converter output with the native runtime.
    if (argc == 5) {
        sse_snapshot_gru::Model direct_baseline;
        assert(direct_baseline.load(argv[1], argv[2], 36U, &error));
        Ensemble converted;
        assert(converted.load(argv[1], argv[2], argv[3], argv[4], &error));
        DualState converted_state;
        Prediction converted_prediction;
        std::vector<float> deterministic_snapshot(36U, 0.0f);
        std::vector<float> deterministic_enhanced(95U, 0.0f);
        for (std::size_t i = 0; i < deterministic_snapshot.size(); ++i) {
            deterministic_snapshot[i] = static_cast<float>(i) * 0.001f - 0.017f;
            deterministic_enhanced[i] = deterministic_snapshot[i];
        }
        for (std::size_t i = 36U; i < deterministic_enhanced.size(); ++i) {
            deterministic_enhanced[i] = static_cast<float>(i - 36U) * 0.002f;
        }
        sse_snapshot_gru::State direct_state;
        float direct_prediction = 0.0f;
        assert(direct_baseline.predict(deterministic_snapshot, &direct_state,
                                       &direct_prediction, &error));
        std::cout << std::setprecision(9) << "direct=" << direct_prediction
                  << " rows=" << direct_state.accepted_rows
                  << " features=" << direct_baseline.feature_count() << "\n";
        assert(converted.predict(deterministic_snapshot, deterministic_enhanced, "sse", 34200000000ULL,
                                 &converted_state, &converted_prediction, &error));
        assert(converted_prediction.valid && std::isfinite(converted_prediction.baseline_pred));
        assert(converted_prediction.valid && std::isfinite(converted_prediction.auction59_pred));
        const float expected_blend = static_cast<float>(
            0.25 * static_cast<double>(converted_prediction.baseline_pred) +
            0.75 * static_cast<double>(converted_prediction.auction59_pred));
        assert(converted_prediction.ensemble_pred == expected_blend);
        std::cout << std::setprecision(9)
                  << "baseline=" << converted_prediction.baseline_pred
                  << " auction59=" << converted_prediction.auction59_pred
                  << " ensemble=" << converted_prediction.ensemble_pred << "\n";

        converted_state.reset();
        std::vector<float> nan_snapshot(36U, std::numeric_limits<float>::quiet_NaN());
        std::vector<float> nan_enhanced(95U, std::numeric_limits<float>::quiet_NaN());
        assert(converted.predict(nan_snapshot, nan_enhanced, "sse", 34200000000ULL,
                                 &converted_state, &converted_prediction, &error));
        assert(converted_prediction.valid && std::isfinite(converted_prediction.baseline_pred));
        assert(converted_prediction.valid && std::isfinite(converted_prediction.auction59_pred));
    }
    return 0;
}
