// Quantify the inference impact of factor approximations on full recurrent
// sequences. Predictions are diagnostics, not executed trading decisions.
#include "common/model/mix153060/mix153060_model.h"
#include "third_party/nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

static std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    std::istringstream input(line);
    std::string value;
    while (std::getline(input, value, ',')) result.push_back(value);
    if (result.size() != 58) throw std::runtime_error("invalid probe row");
    return result;
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: compare BASE.csv NEW.csv MODEL");
        std::ifstream base(argv[1]), candidate(argv[2]);
        if (!base || !candidate) throw std::runtime_error("cannot open samples");
        mix153060::Model model;
        std::string error;
        if (!model.load(argv[3], &error)) throw std::runtime_error(error);
        std::map<std::string, std::pair<mix153060::State, mix153060::State> > states;
        std::vector<double> differences;
        double sum = 0, square_sum = 0, maximum_base = 0;
        size_t sign_changes = 0;
        std::string a, b;
        while (std::getline(base, a)) {
            if (!std::getline(candidate, b)) throw std::runtime_error("sample count mismatch");
            const auto left = fields(a), right = fields(b);
            for (size_t i = 0; i < 8; ++i)
                if (left[i] != right[i]) throw std::runtime_error("sample identity mismatch");
            std::array<float, 50> x, y;
            for (size_t i = 0; i < 50; ++i) {
                x[i] = std::stof(left[8+i]); y[i] = std::stof(right[8+i]);
                if (!std::isfinite(x[i]) || !std::isfinite(y[i]))
                    throw std::runtime_error("nonfinite factor");
            }
            auto& state = states[left[0]];
            float original = 0, changed = 0;
            if (!model.predict(x, &state.first, &original) ||
                !model.predict(y, &state.second, &changed) ||
                !std::isfinite(original) || !std::isfinite(changed))
                throw std::runtime_error("invalid model prediction");
            const double diff = std::abs(double(original) - changed);
            differences.push_back(diff);
            sum += diff; square_sum += diff * diff;
            maximum_base = std::max(maximum_base, std::abs(double(original)));
            sign_changes += (original > 0) != (changed > 0);
        }
        if (std::getline(candidate, b) || differences.empty())
            throw std::runtime_error("extra candidate rows or empty sequence");
        std::sort(differences.begin(), differences.end());
        const size_t n = differences.size();
        std::cout << nlohmann::json{{"samples",n},{"instruments",states.size()},
            {"prediction_abs_error_p50",differences[(n-1)/2]},
            {"prediction_abs_error_p95",differences[(n-1)*95/100]},
            {"prediction_abs_error_p99",differences[(n-1)*99/100]},
            {"prediction_abs_error_max",differences.back()},
            {"prediction_abs_error_mean",sum/n},{"prediction_error_rmse",std::sqrt(square_sum/n)},
            {"baseline_prediction_max_abs",maximum_base},{"prediction_sign_changes",sign_changes},
            {"unit","raw model output; not price units or executed trade changes"}}.dump() << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
