#include "tests/sze/mix153060_golden_fixture.h"
#include <algorithm>
#include <iostream>
#include <limits>

namespace {
struct Saved {
    float prediction;
    mix153060::State state;
    mix153060::Trace trace;
};
}

int main(int argc, char** argv) {
    using namespace mix153060_test;
    try {
        check(argc == 3, "usage: mix153060_sequence_test MODEL GOLDEN");
        const Fixture fixture = load_fixture(argv[2]);
        mix153060::Model model;
        std::string error;
        if (!model.load(argv[1], &error)) throw std::runtime_error(error);
        std::vector<std::vector<Saved> > serial(fixture.size());
        std::size_t total = 0, longest = 0;
        float max_error = 0;
        for (std::size_t group = 0; group < fixture.size(); ++group) {
            mix153060::State state;
            longest = std::max(longest, fixture[group].size());
            for (const Row& row : fixture[group]) {
                Saved saved;
                check(model.predict(row.factors, &state, &saved.prediction, &saved.trace), "serial prediction failed");
                max_error = std::max(max_error, prediction_error(saved.prediction, row.expected));
                saved.state = state;
                serial[group].push_back(saved);
                ++total;
            }
            check(state.accepted_rows == fixture[group].size(), "serial row count mismatch");
        }

        std::vector<mix153060::State> states(fixture.size());
        for (std::size_t row = 0; row < longest; ++row) {
            for (std::size_t group = 0; group < fixture.size(); ++group) {
                if (row >= fixture[group].size()) continue;
                float prediction = 0;
                mix153060::Trace trace;
                check(model.predict(fixture[group][row].factors, &states[group], &prediction, &trace),
                      "interleaved prediction failed");
                const Saved& expected = serial[group][row];
                check(prediction == expected.prediction && states[group].hidden == expected.state.hidden &&
                      states[group].accepted_rows == row + 1, "interleaving changed instrument state");
                check(trace.layernorm_output == expected.trace.layernorm_output &&
                      trace.projected_input == expected.trace.projected_input &&
                      trace.gru_output == expected.trace.gru_output, "interleaving changed layer trace");
            }
        }

        for (std::size_t group = 0; group < fixture.size(); ++group) {
            states[group].reset();
            check(states[group].accepted_rows == 0 && states[group].hidden == mix153060::State().hidden,
                  "reset did not clear state");
            for (std::size_t row = 0; row < fixture[group].size(); ++row) {
                float prediction = 0;
                check(model.predict(fixture[group][row].factors, &states[group], &prediction), "reset replay failed");
                check(prediction == serial[group][row].prediction &&
                      states[group].hidden == serial[group][row].state.hidden &&
                      states[group].accepted_rows == row + 1, "reset changed sequence");
            }
            const mix153060::State before = states[group];
            std::array<float, mix153060::kFeatureCount> invalid = fixture[group][0].factors;
            invalid.back() = std::numeric_limits<float>::quiet_NaN();
            float prediction = 0;
            check(!model.predict(invalid, &states[group], &prediction), "accepted non-finite factor");
            check(states[group].hidden == before.hidden && states[group].accepted_rows == before.accepted_rows,
                  "invalid input changed state");
        }
        std::cout << "groups=" << fixture.size() << " rows=" << total
                  << " max_abs_error=" << max_error << " interleaving=pass reset=pass invalid_input=pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
