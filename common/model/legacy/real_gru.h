#ifndef REAL_GRU_H
#define REAL_GRU_H

#include "third_party/eigen3/Eigen/Dense"
#include "common/config/simple_json_parser.h"
#include <string>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdint>

class RealGRU {
public:
    static constexpr int HIDDEN_DIM = 64;
    static constexpr int FEATURE1_OUT = 512;
    static constexpr int FEATURE2_OUT = 256;
    static constexpr int FEATURE3_OUT = 128;
    static constexpr int GRU_GATES = 192;
    static constexpr int PRED1_OUT = 8;
    static constexpr int PRED2_OUT = 1;

    RealGRU(const std::string& weights_path = "gru_weights.bin", int input_dim = 30) 
        : input_dim_(input_dim) {
        load_weights(weights_path);
    }

    inline Eigen::VectorXf forward(const Eigen::VectorXf& input, Eigen::VectorXf& hidden_state) {
        auto total_start = std::chrono::steady_clock::now();
        auto feat_start = total_start;
        Eigen::VectorXf x1 = (feature1_weight_ * input + feature1_bias_).cwiseMax(0.0f);
        Eigen::VectorXf x2 = (feature2_weight_ * x1 + feature2_bias_).cwiseMax(0.0f);
        Eigen::VectorXf x3 = (feature3_weight_ * x2 + feature3_bias_).cwiseMax(0.0f);
        auto feat_end = std::chrono::steady_clock::now();

        auto gru_start = feat_end;
        Eigen::VectorXf gru_out, hidden_next;
        gru_cell(x3, hidden_state, gru_out, hidden_next);

        hidden_state = hidden_next;
        auto gru_end = std::chrono::steady_clock::now();

        auto head_start = gru_end;
        Eigen::VectorXf p1 = (pred1_weight_ * hidden_next + pred1_bias_).cwiseMax(0.0f);
        Eigen::VectorXf output = pred2_weight_ * p1 + pred2_bias_;
        auto head_end = std::chrono::steady_clock::now();

        timing_stats().add(
            to_ns(head_end - total_start),
            to_ns(feat_end - feat_start),
            to_ns(gru_end - gru_start),
            to_ns(head_end - head_start));

        return output;
    }

private:
    struct ForwardTimingStats {
        std::atomic<uint64_t> total_ns{0};
        std::atomic<uint64_t> feat_ns{0};
        std::atomic<uint64_t> gru_ns{0};
        std::atomic<uint64_t> head_ns{0};
        std::atomic<uint64_t> count{0};

        void add(uint64_t total, uint64_t feat, uint64_t gru, uint64_t head) {
            total_ns.fetch_add(total, std::memory_order_relaxed);
            feat_ns.fetch_add(feat, std::memory_order_relaxed);
            gru_ns.fetch_add(gru, std::memory_order_relaxed);
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
            const uint64_t head_avg = head_ns.load(std::memory_order_relaxed) / calls;
            std::cout << "[Timing][RealGRU::forward] total_avg_ns=" << total_avg
                      << " feat_avg_ns=" << feat_avg
                      << " gru_avg_ns=" << gru_avg
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

    Eigen::MatrixXf pred1_weight_;
    Eigen::VectorXf pred1_bias_;
    Eigen::MatrixXf pred2_weight_;
    Eigen::VectorXf pred2_bias_;

    // inline void gru_cell(const Eigen::VectorXf& x_t, const Eigen::VectorXf& h_prev,
    //                    Eigen::VectorXf& gru_out, Eigen::VectorXf& h_next) {
    //     Eigen::VectorXf gates = gru_weight_ih_ * x_t + gru_bias_ih_ +
    //                          gru_weight_hh_ * h_prev + gru_bias_hh_;
    //
    //     Eigen::VectorXf r_t = gates.segment(0, HIDDEN_DIM);
    //     Eigen::VectorXf z_t = gates.segment(HIDDEN_DIM, HIDDEN_DIM);
    //     Eigen::VectorXf n_t = gates.segment(2 * HIDDEN_DIM, HIDDEN_DIM);
    //
    //     r_t = r_t.unaryExpr([](float x) { return 1.0f / (1.0f + std::exp(-x)); });
    //     z_t = z_t.unaryExpr([](float x) { return 1.0f / (1.0f + std::exp(-x)); });
    //     n_t = n_t.unaryExpr([](float x) { return std::tanh(x); });
    //
    //     h_next = (Eigen::VectorXf::Ones(HIDDEN_DIM) - z_t).cwiseProduct(n_t) + z_t.cwiseProduct(h_prev);
    //
    //     gru_out = h_next;
    // }

    inline void gru_cell(const Eigen::VectorXf& x_t, const Eigen::VectorXf& h_prev,
                   Eigen::VectorXf& gru_out, Eigen::VectorXf& h_next) {

    // --- 步骤 1: 独立计算 R 和 Z 门控 ---
    // GRU 权重通常按 r, z, n 的顺序排列 (W_r, W_z, W_n) / (U_r, U_z, U_n)
    // 假设 gru_weight_ih_ 和 gru_weight_hh_ 的前 2*HIDDEN_DIM 行对应 r 和 z 门

    // 计算 R 和 Z 门的线性部分（输入 x_t 的部分）
    Eigen::VectorXf rz_ih = gru_weight_ih_.topRows(2 * HIDDEN_DIM) * x_t + gru_bias_ih_.topRows(2 * HIDDEN_DIM);
    // 计算 R 和 Z 门的线性部分（先前隐藏状态 h_prev 的部分）
    Eigen::VectorXf rz_hh = gru_weight_hh_.topRows(2 * HIDDEN_DIM) * h_prev + gru_bias_hh_.topRows(2 * HIDDEN_DIM);

    // 组合 R 和 Z 门控
    Eigen::VectorXf rz_gates = rz_ih + rz_hh;

    // 分离 R 门和 Z 门
    Eigen::VectorXf r_t = rz_gates.segment(0, HIDDEN_DIM);
    Eigen::VectorXf z_t = rz_gates.segment(HIDDEN_DIM, HIDDEN_DIM);

    // 应用激活函数
    auto sigmoid = [](float x) { return 1.0f / (1.0f + std::exp(-x)); };
    r_t = r_t.unaryExpr(sigmoid);
    z_t = z_t.unaryExpr(sigmoid);


    // --- 步骤 2: 计算 候选隐状态 n_t (包含重置门) ---
    // 计算候选隐状态 n 的线性部分（输入 x_t 的部分）
    // 假设 gru_weight_ih_ 的后 HIDDEN_DIM 行对应 n 门 (W_n)
    Eigen::VectorXf n_ih = gru_weight_ih_.bottomRows(HIDDEN_DIM) * x_t + gru_bias_ih_.bottomRows(HIDDEN_DIM);

    // 计算候选隐状态 n 的线性部分（先前隐藏状态 h_prev 的部分）
    // 假设 gru_weight_hh_ 的后 HIDDEN_DIM 行对应 n 门 (U_n)
    Eigen::VectorXf n_hh_unreset = gru_weight_hh_.bottomRows(HIDDEN_DIM) * h_prev + gru_bias_hh_.bottomRows(HIDDEN_DIM);

    // 核心修改：使用重置门 r_t 门控 h_prev 的部分
    Eigen::VectorXf n_hh_reset = r_t.cwiseProduct(n_hh_unreset);

    // 最终的 n 门控输入
    Eigen::VectorXf n_t_input = n_ih + n_hh_reset;

    // 应用 tanh 激活函数
    auto tanh_activation = [](float x) { return std::tanh(x); };
    Eigen::VectorXf n_t = n_t_input.unaryExpr(tanh_activation);


    // --- 步骤 3: 更新隐藏状态 (与原来一致) ---
    // h_t = (1 - z_t) * n_t + z_t * h_{t-1}
    h_next = (Eigen::VectorXf::Ones(HIDDEN_DIM) - z_t).cwiseProduct(n_t) + z_t.cwiseProduct(h_prev);

    // 输出GRU序列
    gru_out = h_next;
}

    void load_weights(const std::string& filename) {
        SimpleJsonParser parser;
        if (!parser.load_from_file(filename)) {
            throw std::runtime_error("Cannot open weights file: " + filename);
        }

        auto weights = parser.parse_weights();
        if (weights.empty()) {
            throw std::runtime_error("Cannot parse weights from JSON file");
        }

        auto load_matrix_from_json = [](Eigen::MatrixXf& matrix, const std::vector<double>& data, int rows, int cols) {
            matrix.resize(rows, cols);
            for (int i = 0; i < rows; ++i) {
                for (int j = 0; j < cols; ++j) {
                    matrix(i, j) = static_cast<float>(data[i * cols + j]);
                }
            }
        };

        auto load_vector_from_json = [](Eigen::VectorXf& vector, const std::vector<double>& data) {
            vector.resize(data.size());
            for (size_t i = 0; i < data.size(); ++i) {
                vector(i) = static_cast<float>(data[i]);
            }
        };

        if (weights.find("feature1") != weights.end()) {
            auto& layer = weights["feature1"];
            if (layer.find("weight") != layer.end()) {
                load_matrix_from_json(feature1_weight_, layer["weight"], FEATURE1_OUT, input_dim_);
            }
            if (layer.find("bias") != layer.end()) {
                load_vector_from_json(feature1_bias_, layer["bias"]);
            }
        }

        if (weights.find("feature2") != weights.end()) {
            auto& layer = weights["feature2"];
            if (layer.find("weight") != layer.end()) {
                load_matrix_from_json(feature2_weight_, layer["weight"], FEATURE2_OUT, FEATURE1_OUT);
            }
            if (layer.find("bias") != layer.end()) {
                load_vector_from_json(feature2_bias_, layer["bias"]);
            }
        }

        if (weights.find("feature3") != weights.end()) {
            auto& layer = weights["feature3"];
            if (layer.find("weight") != layer.end()) {
                load_matrix_from_json(feature3_weight_, layer["weight"], FEATURE3_OUT, FEATURE2_OUT);
            }
            if (layer.find("bias") != layer.end()) {
                load_vector_from_json(feature3_bias_, layer["bias"]);
            }
        }

        if (weights.find("gru") != weights.end()) {
            auto& layer = weights["gru"];
            
            std::string weight_ih_key = "weight_ih";
            if (layer.find("weight_ih_l0") != layer.end()) {
                weight_ih_key = "weight_ih_l0";
            }
            if (layer.find(weight_ih_key) != layer.end()) {
                load_matrix_from_json(gru_weight_ih_, layer[weight_ih_key], GRU_GATES, FEATURE3_OUT);
            }

            std::string weight_hh_key = "weight_hh";
            if (layer.find("weight_hh_l0") != layer.end()) {
                weight_hh_key = "weight_hh_l0";
            }
            if (layer.find(weight_hh_key) != layer.end()) {
                load_matrix_from_json(gru_weight_hh_, layer[weight_hh_key], GRU_GATES, HIDDEN_DIM);
            }

            std::string bias_ih_key = "bias_ih";
            if (layer.find("bias_ih_l0") != layer.end()) {
                bias_ih_key = "bias_ih_l0";
            }
            if (layer.find(bias_ih_key) != layer.end()) {
                load_vector_from_json(gru_bias_ih_, layer[bias_ih_key]);
            }

            std::string bias_hh_key = "bias_hh";
            if (layer.find("bias_hh_l0") != layer.end()) {
                bias_hh_key = "bias_hh_l0";
            }
            if (layer.find(bias_hh_key) != layer.end()) {
                load_vector_from_json(gru_bias_hh_, layer[bias_hh_key]);
            }
        }

        if (weights.find("pred1") != weights.end()) {
            auto& layer = weights["pred1"];
            if (layer.find("weight") != layer.end()) {
                load_matrix_from_json(pred1_weight_, layer["weight"], PRED1_OUT, HIDDEN_DIM);
            }
            if (layer.find("bias") != layer.end()) {
                load_vector_from_json(pred1_bias_, layer["bias"]);
            }
        }

        if (weights.find("pred2") != weights.end()) {
            auto& layer = weights["pred2"];
            if (layer.find("weight") != layer.end()) {
                load_matrix_from_json(pred2_weight_, layer["weight"], PRED2_OUT, PRED1_OUT);
            }
            if (layer.find("bias") != layer.end()) {
                load_vector_from_json(pred2_bias_, layer["bias"]);
            }
        }
    }
};

#endif // REAL_GRU_H
