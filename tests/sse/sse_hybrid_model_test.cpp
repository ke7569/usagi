#include "sse/model/sse_hybrid_model.h"
#include "tests/sse/sse_test_artifacts.h"

#include <array>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <unistd.h>

using sse_test_artifacts::write_scaler;
using sse_test_artifacts::write_snapshot_artifact;
using sse_test_artifacts::write_tick_artifact;

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
    assert(sse_hybrid_model::Model::selected_source("sse", 34499999999ULL) == sse_hybrid_model::kSnapshotSource);
    assert(sse_hybrid_model::Model::selected_source("sse", 34500000000ULL) == sse_hybrid_model::kTickSource);
    assert(sse_hybrid_model::Model::selected_source("sze", 34500000000ULL) == sse_hybrid_model::kNoSource);
    assert(sse_hybrid_model::Model::selected_source("sse", 86400000000ULL) == sse_hybrid_model::kNoSource);

    sse_hybrid_model::State state;
    sse_hybrid_model::Prediction output;
    std::array<float, sse_model::kFeatureCount> tick_factors = {};
    std::vector<float> snapshot(36U, 0.0f);
    std::vector<float> enhanced(95U, 0.0f);
    assert(model.on_tick(tick_factors, "sse", 34200000000ULL, &state, &output, &error));
    assert(output.tick_generated && !output.selected && state.tick.accepted_rows == 1U);
    assert(model.on_snapshot(snapshot, enhanced, "sse", 34200000001ULL, &state, &output, &error));
    assert(output.snapshot_generated && output.selected && output.selected_source == sse_hybrid_model::kSnapshotSource && state.snapshot.baseline.accepted_rows == 1U && state.snapshot.auction59.accepted_rows == 1U);
    assert(model.on_tick(tick_factors, "sse", 34500000000ULL, &state, &output, &error));
    assert(output.tick_generated && output.selected && output.selected_source == sse_hybrid_model::kTickSource && output.selected_pred == 1.5f && state.tick.accepted_rows == 2U);
    const std::uint64_t snapshot_rows = state.snapshot.baseline.accepted_rows;
    assert(model.on_snapshot(snapshot, enhanced, "sse", 34500000000ULL, &state, &output, &error));
    assert(output.snapshot_generated && !output.selected);
    assert(model.on_snapshot(snapshot, enhanced, "sse", 34859999999ULL, &state, &output, &error));
    assert(output.snapshot_generated && !output.selected);
    assert(!model.on_snapshot(snapshot, enhanced, "sse", 34860000000ULL, &state, &output, &error));
    assert(state.snapshot.baseline.accepted_rows == snapshot_rows + 2);
    tick_factors[0] = std::numeric_limits<float>::infinity();
    assert(!model.on_tick(tick_factors, "sse", 34500000001ULL, &state, &output, &error));
    assert(state.tick.accepted_rows == 2U);

    if (argc == 6) {
        sse_hybrid_model::Model actual;
        assert(actual.load(argv[1], argv[2], argv[3], argv[4], argv[5], &error));
        sse_hybrid_model::State actual_state;
        sse_hybrid_model::Prediction actual_output;
        std::array<float, sse_model::kFeatureCount> actual_tick = {};
        std::vector<float> actual_snapshot(36U, 0.0f);
        std::vector<float> actual_enhanced(95U, 0.0f);
        assert(actual.on_tick(actual_tick, "sse", 34200000000ULL, &actual_state, &actual_output, &error));
        assert(actual_output.tick_generated && !actual_output.selected);
        const float opening_tick = actual_output.tick_pred;
        assert(actual.on_snapshot(actual_snapshot, actual_enhanced, "sse", 34200000001ULL, &actual_state, &actual_output, &error));
        assert(actual_output.selected_source == sse_hybrid_model::kSnapshotSource);
        const float opening_snapshot = actual_output.selected_pred;
        assert(actual.on_tick(actual_tick, "sse", 34500000000ULL, &actual_state, &actual_output, &error));
        assert(actual_output.selected_source == sse_hybrid_model::kTickSource);
        std::cout << std::setprecision(9) << "opening_tick_shadow=" << opening_tick << " opening_snapshot_selected=" << opening_snapshot << " post_switch_tick_selected=" << actual_output.selected_pred << " tick_rows=" << actual_state.tick.accepted_rows << "\n";
    }
    return 0;
}
