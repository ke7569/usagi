#ifndef GRU_VER_1_H
#define GRU_VER_1_H

#include "third_party/eigen3/Eigen/Dense"
#include "third_party/nlohmann/json.hpp"
#include <string>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <chrono>
#include <atomic>
#include <cstdint>

class gru_ver_1 {
public:
    static constexpr float LAYER_NORM_EPS = 1e-5f;
    static constexpr int PRED1_OUT = 8;
    static constexpr int PRED2_OUT = 1;

    gru_ver_1(const std::string& weights_path = "model/model_1.json",
              int input_dim = 30,
              int hidden_dim = 64)
        : input_dim_(input_dim),
          hidden_dim_(hidden_dim),
          feature1_out_(hidden_dim * 8),
          feature2_out_(hidden_dim * 4),
          feature3_out_(hidden_dim * 2) {
        load_weights(weights_path);
    }

    inline Eigen::VectorXf forward(const Eigen::VectorXf& input,
                                   Eigen::VectorXf& hidden_state) {
        auto total_start = std::chrono::steady_clock::now();
        auto feat_start = total_start;
        SoftsignOp softsign;
        Eigen::VectorXf x1 = (feature1_weight_ * input + feature1_bias_).unaryExpr(softsign);
        Eigen::VectorXf x2 = (feature2_weight_ * x1 + feature2_bias_).unaryExpr(softsign);
        Eigen::VectorXf x3 = (feature3_weight_ * x2 + feature3_bias_).unaryExpr(softsign);

        Eigen::VectorXf proj = (proj_weight_ * input + proj_bias_);
        Eigen::VectorXf feat = x3 + proj;
        auto feat_end = std::chrono::steady_clock::now();

        auto gru_start = feat_end;
        Eigen::VectorXf gru_out, hidden_next;
        gru_cell(feat, hidden_state, gru_out, hidden_next);
        hidden_state = hidden_next;
        auto gru_end = std::chrono::steady_clock::now();

        auto norm_start = gru_end;
        Eigen::VectorXf res = res_gru_weight_ * feat + res_gru_bias_;
        Eigen::VectorXf ln_out = layer_norm(gru_out + res);
        auto norm_end = std::chrono::steady_clock::now();

        auto head_start = norm_end;
        Eigen::VectorXf p1 = (pred1_weight_ * ln_out + pred1_bias_).unaryExpr(softsign);
        Eigen::VectorXf output = pred2_weight_ * p1 + pred2_bias_;
        auto head_end = std::chrono::steady_clock::now();

        timing_stats().add(
            to_ns(head_end - total_start),
            to_ns(feat_end - feat_start),
            to_ns(gru_end - gru_start),
            to_ns(norm_end - norm_start),
            to_ns(head_end - head_start));
        return output;
    }

private:
    struct ForwardTimingStats {
        std::atomic<uint64_t> total_ns{0};
        std::atomic<uint64_t> feat_ns{0};
        std::atomic<uint64_t> gru_ns{0};
        std::atomic<uint64_t> norm_ns{0};
        std::atomic<uint64_t> head_ns{0};
        std::atomic<uint64_t> count{0};

        void add(uint64_t total, uint64_t feat, uint64_t gru, uint64_t norm, uint64_t head) {
            total_ns.fetch_add(total, std::memory_order_relaxed);
            feat_ns.fetch_add(feat, std::memory_order_relaxed);
            gru_ns.fetch_add(gru, std::memory_order_relaxed);
            norm_ns.fetch_add(norm, std::memory_order_relaxed);
            head_ns.fetch_add(head, std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);
        }

        ~ForwardTimingStats() {
            const uint64_t calls = count.load(std::memory_order_relaxed);
            if (calls == 0) {
                return;
            }
            const uint64_t total_avg = total_ns.load(std::memory_order_relaxed) / calls;
            const uint64_t feat_avg = feat_ns.load(std::memory_order_relaxed) / calls;
            const uint64_t gru_avg = gru_ns.load(std::memory_order_relaxed) / calls;
            const uint64_t norm_avg = norm_ns.load(std::memory_order_relaxed) / calls;
            const uint64_t head_avg = head_ns.load(std::memory_order_relaxed) / calls;
            std::cout << "[Timing][gru_ver_1::forward] total_avg_ns=" << total_avg
                      << " feat_avg_ns=" << feat_avg
                      << " gru_avg_ns=" << gru_avg
                      << " norm_avg_ns=" << norm_avg
                      << " head_avg_ns=" << head_avg
                      << " count=" << calls << std::endl;
        }
    };

    static ForwardTimingStats& timing_stats() {
        static ForwardTimingStats stats;
        return stats;
    }

    static uint64_t to_ns(std::chrono::steady_clock::duration duration) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    }

    int input_dim_;
    int hidden_dim_;
    int feature1_out_;
    int feature2_out_;
    int feature3_out_;

    Eigen::MatrixXf proj_weight_;
    Eigen::VectorXf proj_bias_;

    Eigen::MatrixXf feature1_weight_;
    Eigen::VectorXf feature1_bias_;
    Eigen::MatrixXf feature2_weight_;
    Eigen::VectorXf feature2_bias_;
    Eigen::MatrixXf feature3_weight_;
    Eigen::VectorXf feature3_bias_;

    Eigen::MatrixXf gru_weight_ih_;
    Eigen::VectorXf gru_bias_ih_;
    Eigen::MatrixXf gru_weight_hh_;
    Eigen::VectorXf gru_bias_hh_;

    Eigen::MatrixXf res_gru_weight_;
    Eigen::VectorXf res_gru_bias_;

    Eigen::VectorXf ln_weight_;
    Eigen::VectorXf ln_bias_;

    Eigen::MatrixXf pred1_weight_;
    Eigen::VectorXf pred1_bias_;
    Eigen::MatrixXf pred2_weight_;
    Eigen::VectorXf pred2_bias_;

    struct SoftsignOp {
        float operator()(float x) const {
            return x / (1.0f + std::fabs(x));
        }
    };

        Eigen::VectorXf layer_norm(const Eigen::VectorXf& x) const {
        float mean = x.mean();
        Eigen::ArrayXf centered = x.array() - mean;
        float var = centered.square().mean();
        Eigen::VectorXf norm = (centered / std::sqrt(var + LAYER_NORM_EPS)).matrix();
        if (ln_weight_.size() == x.size() && ln_bias_.size() == x.size()) {
            norm = norm.cwiseProduct(ln_weight_) + ln_bias_;
        }
        return norm;
    }

    inline void gru_cell(const Eigen::VectorXf& x_t, const Eigen::VectorXf& h_prev,
                         Eigen::VectorXf& gru_out, Eigen::VectorXf& h_next) {
        int gate_dim = 2 * hidden_dim_;

        Eigen::VectorXf rz_ih = gru_weight_ih_.topRows(gate_dim) * x_t + gru_bias_ih_.head(gate_dim);
        Eigen::VectorXf rz_hh = gru_weight_hh_.topRows(gate_dim) * h_prev + gru_bias_hh_.head(gate_dim);
        Eigen::VectorXf rz_gates = rz_ih + rz_hh;

        Eigen::VectorXf r_t = rz_gates.segment(0, hidden_dim_);
        Eigen::VectorXf z_t = rz_gates.segment(hidden_dim_, hidden_dim_);

        auto sigmoid = [](float x) { return 1.0f / (1.0f + std::exp(-x)); };
        r_t = r_t.unaryExpr(sigmoid);
        z_t = z_t.unaryExpr(sigmoid);

        Eigen::VectorXf n_ih = gru_weight_ih_.bottomRows(hidden_dim_) * x_t + gru_bias_ih_.tail(hidden_dim_);
        Eigen::VectorXf n_hh_unreset = gru_weight_hh_.bottomRows(hidden_dim_) * h_prev + gru_bias_hh_.tail(hidden_dim_);
        Eigen::VectorXf n_hh_reset = r_t.cwiseProduct(n_hh_unreset);
        Eigen::VectorXf n_t_input = n_ih + n_hh_reset;

        auto tanh_activation = [](float x) { return std::tanh(x); };
        Eigen::VectorXf n_t = n_t_input.unaryExpr(tanh_activation);

        h_next = (Eigen::VectorXf::Ones(hidden_dim_) - z_t).cwiseProduct(n_t) + z_t.cwiseProduct(h_prev);
        gru_out = h_next;
    }

    void load_weights(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open weights file: " + filename);
        }

        nlohmann::json root;
        file >> root;
        if (root.find("weights") == root.end()) {
            throw std::runtime_error("Cannot parse weights from JSON file");
        }

        const auto& weights = root["weights"];
        auto has_key = [](const nlohmann::json& obj, const char* key) {
            return obj.find(key) != obj.end();
        };

        auto load_matrix = [](Eigen::MatrixXf& matrix,
                              const nlohmann::json& data,
                              int rows,
                              int cols) {
            matrix.resize(rows, cols);
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) {
                    matrix(i, j) = data[i][j].get<float>();
                }
            }
        };

        auto load_vector = [](Eigen::VectorXf& vector,
                              const nlohmann::json& data) {
            vector.resize(static_cast<int>(data.size()));
            for (size_t i = 0; i < data.size(); ++i) {
                vector(static_cast<int>(i)) = data[i].get<float>();
            }
        };

        if (has_key(weights, "proj")) {
            const auto& layer = weights["proj"];
            if (has_key(layer, "weight")) {
                load_matrix(proj_weight_, layer["weight"], feature3_out_, input_dim_);
            }
            if (has_key(layer, "bias")) {
                load_vector(proj_bias_, layer["bias"]);
            }
        }

        if (has_key(weights, "feature1")) {
            const auto& layer = weights["feature1"];
            if (has_key(layer, "weight")) {
                load_matrix(feature1_weight_, layer["weight"], feature1_out_, input_dim_);
            }
            if (has_key(layer, "bias")) {
                load_vector(feature1_bias_, layer["bias"]);
            }
        }

        if (has_key(weights, "feature2")) {
            const auto& layer = weights["feature2"];
            if (has_key(layer, "weight")) {
                load_matrix(feature2_weight_, layer["weight"], feature2_out_, feature1_out_);
            }
            if (has_key(layer, "bias")) {
                load_vector(feature2_bias_, layer["bias"]);
            }
        }

        if (has_key(weights, "feature3")) {
            const auto& layer = weights["feature3"];
            if (has_key(layer, "weight")) {
                load_matrix(feature3_weight_, layer["weight"], feature3_out_, feature2_out_);
            }
            if (has_key(layer, "bias")) {
                load_vector(feature3_bias_, layer["bias"]);
            }
        }

        if (has_key(weights, "gru")) {
            const auto& layer = weights["gru"];

            std::string weight_ih_key = has_key(layer, "weight_ih_l0") ? "weight_ih_l0" : "weight_ih";
            if (has_key(layer, weight_ih_key.c_str())) {
                load_matrix(gru_weight_ih_, layer[weight_ih_key], 3 * hidden_dim_, feature3_out_);
            }

            std::string weight_hh_key = has_key(layer, "weight_hh_l0") ? "weight_hh_l0" : "weight_hh";
            if (has_key(layer, weight_hh_key.c_str())) {
                load_matrix(gru_weight_hh_, layer[weight_hh_key], 3 * hidden_dim_, hidden_dim_);
            }

            std::string bias_ih_key = has_key(layer, "bias_ih_l0") ? "bias_ih_l0" : "bias_ih";
            if (has_key(layer, bias_ih_key.c_str())) {
                load_vector(gru_bias_ih_, layer[bias_ih_key]);
            }

            std::string bias_hh_key = has_key(layer, "bias_hh_l0") ? "bias_hh_l0" : "bias_hh";
            if (has_key(layer, bias_hh_key.c_str())) {
                load_vector(gru_bias_hh_, layer[bias_hh_key]);
            }
        }

        if (has_key(weights, "res_gru")) {
            const auto& layer = weights["res_gru"];
            if (has_key(layer, "weight")) {
                load_matrix(res_gru_weight_, layer["weight"], hidden_dim_, feature3_out_);
            }
            if (has_key(layer, "bias")) {
                load_vector(res_gru_bias_, layer["bias"]);
            }
        }

        if (has_key(weights, "ln")) {
            const auto& layer = weights["ln"];
            if (has_key(layer, "weight")) {
                load_vector(ln_weight_, layer["weight"]);
            }
            if (has_key(layer, "bias")) {
                load_vector(ln_bias_, layer["bias"]);
            }
        }

        if (has_key(weights, "pred1")) {
            const auto& layer = weights["pred1"];
            if (has_key(layer, "weight")) {
                load_matrix(pred1_weight_, layer["weight"], PRED1_OUT, hidden_dim_);
            }
            if (has_key(layer, "bias")) {
                load_vector(pred1_bias_, layer["bias"]);
            }
        }

        if (has_key(weights, "pred2")) {
            const auto& layer = weights["pred2"];
            if (has_key(layer, "weight")) {
                load_matrix(pred2_weight_, layer["weight"], PRED2_OUT, PRED1_OUT);
            }
            if (has_key(layer, "bias")) {
                load_vector(pred2_bias_, layer["bias"]);
            }
        }
    }
};

#endif // GRU_VER_1_H
