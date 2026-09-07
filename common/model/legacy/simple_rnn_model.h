#ifndef SIMPLE_RNN_MODEL_H
#define SIMPLE_RNN_MODEL_H

#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <cmath>
#include <random>
#include <memory>
#include "common/config/simple_json_parser.h"

class SimpleRNNModel {
private:
    // 模型参数
    int n_feature;
    int n_rnn_neuron;
    std::string dense_activation;
    std::string rnn_activation;
    float dropout_rate;
    
    // 特征提取层权重和偏置
    std::vector<std::vector<float>> feature1_weight, feature2_weight, feature3_weight;
    std::vector<float> feature1_bias, feature2_bias, feature3_bias;
    
    // GRU 权重和偏置
    std::vector<std::vector<float>> gru_weight_ih, gru_weight_hh;
    std::vector<float> gru_bias_ih, gru_bias_hh;
    
    // 输出层权重和偏置
    std::vector<std::vector<float>> pred1_weight, pred2_weight;
    std::vector<float> pred1_bias, pred2_bias;
    
    // 隐状态
    std::vector<float> hidden_state_;
    
    // 预测计数器
    static int prediction_count;
    
    // 辅助函数
    float relu(float x) { return x > 0 ? x : 0; }
    float tanh_activation(float x) { return std::tanh(x); }
    float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

    
    // 矩阵向量乘法
    std::vector<float> mat_vec_mul(const std::vector<std::vector<float>>& mat, const std::vector<float>& vec) {
        std::vector<float> result(mat.size(), 0.0f);
        for (size_t i = 0; i < mat.size(); ++i) {
            for (size_t j = 0; j < vec.size(); ++j) {
                result[i] += mat[i][j] * vec[j];
            }
        }
        return result;
    }
    
    // 向量加法
    std::vector<float> vec_add(const std::vector<float>& a, const std::vector<float>& b) {
        std::vector<float> result(a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            result[i] = a[i] + b[i];
        }
        return result;
    }
    
    // 向量元素乘法
    std::vector<float> vec_mul_elem(const std::vector<float>& a, const std::vector<float>& b) {
        std::vector<float> result(a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            result[i] = a[i] * b[i];
        }
        return result;
    }
    
    // GRU前向传播 (标准GRU实现)
    std::vector<float> gru_forward(const std::vector<float>& input) {
        int hidden_size = n_rnn_neuron;
        
        // 重置门 (reset gate): r_t = sigmoid(W_ir @ x_t + b_ir + W_hr @ h_{t-1} + b_hr)
        std::vector<float> reset_gate(hidden_size);
        for (int i = 0; i < hidden_size; ++i) {
            float sum = gru_bias_ih[i]; // b_ir
            for (size_t j = 0; j < input.size(); ++j) {
                sum += gru_weight_ih[i][j] * input[j]; // W_ir @ x_t
            }
            sum += gru_bias_hh[i]; // b_hr
            for (size_t j = 0; j < hidden_state_.size(); ++j) {
                sum += gru_weight_hh[i][j] * hidden_state_[j]; // W_hr @ h_{t-1}
            }
            reset_gate[i] = sigmoid(sum);
        }
        
        // 更新门 (update gate): z_t = sigmoid(W_iz @ x_t + b_iz + W_hz @ h_{t-1} + b_hz)
        std::vector<float> update_gate(hidden_size);
        for (int i = 0; i < hidden_size; ++i) {
            float sum = gru_bias_ih[hidden_size + i]; // b_iz
            for (size_t j = 0; j < input.size(); ++j) {
                sum += gru_weight_ih[hidden_size + i][j] * input[j]; // W_iz @ x_t
            }
            sum += gru_bias_hh[hidden_size + i]; // b_hz
            for (size_t j = 0; j < hidden_state_.size(); ++j) {
                sum += gru_weight_hh[hidden_size + i][j] * hidden_state_[j]; // W_hz @ h_{t-1}
            }
            update_gate[i] = sigmoid(sum);
        }
        
        // 候选隐状态: n_t = tanh(W_in @ x_t + b_in + r_t * (W_hn @ h_{t-1} + b_hn))
        std::vector<float> candidate(hidden_size);
        for (int i = 0; i < hidden_size; ++i) {
            float sum = gru_bias_ih[2 * hidden_size + i]; // b_in
            for (size_t j = 0; j < input.size(); ++j) {
                sum += gru_weight_ih[2 * hidden_size + i][j] * input[j]; // W_in @ x_t
            }
            
            float hidden_part = gru_bias_hh[2 * hidden_size + i]; // b_hn
            for (size_t j = 0; j < hidden_state_.size(); ++j) {
                hidden_part += gru_weight_hh[2 * hidden_size + i][j] * hidden_state_[j]; // W_hn @ h_{t-1}
            }
            
            candidate[i] = tanh_activation(sum + reset_gate[i] * hidden_part);
        }
        
        // 新隐状态: h_t = (1 - z_t) * n_t + z_t * h_{t-1}
        std::vector<float> new_hidden(hidden_size);
        for (int i = 0; i < hidden_size; ++i) {
            new_hidden[i] = (1.0f - update_gate[i]) * candidate[i] + update_gate[i] * hidden_state_[i];
        }
        
        hidden_state_ = new_hidden;
        return new_hidden;
    }

public:
    SimpleRNNModel(int n_feat = 30, int n_rnn_neur = 64, const std::string& dense_act = "relu", 
                   const std::string& rnn_act = "tanh", float dropout_r = 0.2f)
        : n_feature(n_feat), n_rnn_neuron(n_rnn_neur), dense_activation(dense_act), 
          rnn_activation(rnn_act), dropout_rate(dropout_r) {
        
        // 初始化权重（随机）
        initialize_weights();
        
        // 初始化隐状态
        hidden_state_.resize(n_rnn_neuron, 0.0f);
    }
    
    void initialize_weights() {
        // 特征提取层
        feature1_weight.resize(n_rnn_neuron * 8, std::vector<float>(n_feature));
        feature1_bias.resize(n_rnn_neuron * 8);
        feature2_weight.resize(n_rnn_neuron * 4, std::vector<float>(n_rnn_neuron * 8));
        feature2_bias.resize(n_rnn_neuron * 4);
        feature3_weight.resize(n_rnn_neuron * 2, std::vector<float>(n_rnn_neuron * 4));
        feature3_bias.resize(n_rnn_neuron * 2);
        
        // GRU层 - 输入权重 (3*hidden_size, input_size)
        gru_weight_ih.resize(3 * n_rnn_neuron, std::vector<float>(n_rnn_neuron * 2));
        // GRU层 - 隐状态权重 (3*hidden_size, hidden_size)  
        gru_weight_hh.resize(3 * n_rnn_neuron, std::vector<float>(n_rnn_neuron));
        gru_bias_ih.resize(3 * n_rnn_neuron);
        gru_bias_hh.resize(3 * n_rnn_neuron);
        
        // 输出层
        pred1_weight.resize(8, std::vector<float>(n_rnn_neuron));
        pred1_bias.resize(8);
        pred2_weight.resize(1, std::vector<float>(8));
        pred2_bias.resize(1);
        
        // 使用Xavier初始化
        std::random_device rd;
        std::mt19937 gen(rd());
        
        auto init_2d = [&](std::vector<std::vector<float>>& w, int fan_in, int fan_out) {
            float limit = std::sqrt(6.0f / (fan_in + fan_out));
            std::uniform_real_distribution<float> dis(-limit, limit);
            for (auto& row : w) {
                for (float& val : row) {
                    val = dis(gen);
                }
            }
        };
        
        auto init_1d = [&](std::vector<float>& b) {
            std::uniform_real_distribution<float> dis(-0.1f, 0.1f);
            for (float& val : b) {
                val = dis(gen);
            }
        };
        
        init_2d(feature1_weight, n_feature, n_rnn_neuron * 8);
        init_1d(feature1_bias);
        init_2d(feature2_weight, n_rnn_neuron * 8, n_rnn_neuron * 4);
        init_1d(feature2_bias);
        init_2d(feature3_weight, n_rnn_neuron * 4, n_rnn_neuron * 2);
        init_1d(feature3_bias);
        init_2d(gru_weight_ih, n_rnn_neuron * 2, 3 * n_rnn_neuron);
        init_2d(gru_weight_hh, n_rnn_neuron, 3 * n_rnn_neuron);
        init_1d(gru_bias_ih);
        init_1d(gru_bias_hh);
        init_2d(pred1_weight, n_rnn_neuron, 8);
        init_1d(pred1_bias);
        init_2d(pred2_weight, 8, 1);
        init_1d(pred2_bias);
    }
    
    // 单步预测方法（维护隐状态）
    float predict_single_step(const std::vector<float>& input) {
        if (input.size() != n_feature) {
            std::cerr << "输入特征维度不匹配: 期望 " << n_feature << ", 实际 " << input.size() << std::endl;
            return 0.0f;
        }
        
        for (int i = 0; i < std::min(20, (int)input.size()); ++i) {
        }
        
        // 特征提取层1
        std::vector<float> f1_out = vec_add(mat_vec_mul(feature1_weight, input), feature1_bias);
        for (float& val : f1_out) val = relu(val);
        for (int i = 0; i < std::min(20, (int)f1_out.size()); ++i) {
        }

        // 特征提取层2
        std::vector<float> f2_out = vec_add(mat_vec_mul(feature2_weight, f1_out), feature2_bias);
        for (float& val : f2_out) val = relu(val);
        for (int i = 0; i < std::min(20, (int)f2_out.size()); ++i) {
        }

        // 特征提取层3
        std::vector<float> f3_out = vec_add(mat_vec_mul(feature3_weight, f2_out), feature3_bias);
        for (float& val : f3_out) val = relu(val);
        for (int i = 0; i < std::min(20, (int)f3_out.size()); ++i) {
        }

        // GRU层（更新隐状态）
        std::vector<float> gru_output = gru_forward(f3_out);
        for (int i = 0; i < std::min(20, (int)gru_output.size()); ++i) {
        }

        // 输出层1
        std::vector<float> p1_out = vec_add(mat_vec_mul(pred1_weight, gru_output), pred1_bias);
        for (float& val : p1_out) val = relu(val);
        for (int i = 0; i < std::min(20, (int)p1_out.size()); ++i) {
        }
        
        // 输出层2
        std::vector<float> p2_out = vec_add(mat_vec_mul(pred2_weight, p1_out), pred2_bias);

        return p2_out[0];
    }
    
    // 重置隐状态
    void reset_hidden_state() {
        hidden_state_.assign(n_rnn_neuron, 0.0f);
    }
    
    // 获取当前隐状态
    const std::vector<float>& get_hidden_state() const {
        return hidden_state_;
    }

    // 从 JSON 文件加载权重
    bool load_weights(const std::string& filename) {
        SimpleJsonParser parser;
        if (!parser.load_from_file(filename)) {
            std::cerr << "无法加载权重文件: " << filename << std::endl;
            return false;
        }
        
        // 解析模型配置
        auto config = parser.parse_model_config();
        
        // 解析权重
        auto weights = parser.parse_weights();
        if (weights.empty()) {
            std::cerr << "无法解析权重数据" << std::endl;
            return false;
        }
        
        // 加载特征提取层权重
        if (weights.find("feature1") != weights.end()) {
            auto& layer = weights["feature1"];
            if (layer.find("weight") != layer.end()) {
                auto& weight_data = layer["weight"];
                
                // 正确加载2D权重矩阵
                size_t expected_size = feature1_weight.size() * feature1_weight[0].size();
                if (weight_data.size() >= expected_size) {
                    for (size_t i = 0; i < feature1_weight.size(); ++i) {
                        for (size_t j = 0; j < feature1_weight[i].size(); ++j) {
                            feature1_weight[i][j] = weight_data[i * feature1_weight[i].size() + j];
                        }
                    }
                } else {
                    std::cerr << "Feature1权重数据大小不匹配: 期望 " << expected_size << ", 实际 " << weight_data.size() << std::endl;
                }
            }
            if (layer.find("bias") != layer.end()) {
                auto& bias_data = layer["bias"];
                for (size_t i = 0; i < feature1_bias.size() && i < bias_data.size(); ++i) {
                    feature1_bias[i] = bias_data[i];
                }
            }
        }
        
        // 加载Feature2权重
        if (weights.find("feature2") != weights.end()) {
            auto& layer = weights["feature2"];
            if (layer.find("weight") != layer.end()) {
                auto& weight_data = layer["weight"];
                size_t expected_size = feature2_weight.size() * feature2_weight[0].size();
                if (weight_data.size() >= expected_size) {
                    for (size_t i = 0; i < feature2_weight.size(); ++i) {
                        for (size_t j = 0; j < feature2_weight[i].size(); ++j) {
                            feature2_weight[i][j] = weight_data[i * feature2_weight[i].size() + j];
                        }
                    }
                } else {
                    std::cerr << "Feature2权重数据大小不匹配: 期望 " << expected_size << ", 实际 " << weight_data.size() << std::endl;
                }
            }
            if (layer.find("bias") != layer.end()) {
                auto& bias_data = layer["bias"];
                for (size_t i = 0; i < feature2_bias.size() && i < bias_data.size(); ++i) {
                    feature2_bias[i] = bias_data[i];
                }
            }
        }
        
        // 加载Feature3权重
        if (weights.find("feature3") != weights.end()) {
            auto& layer = weights["feature3"];
            if (layer.find("weight") != layer.end()) {
                auto& weight_data = layer["weight"];
                size_t expected_size = feature3_weight.size() * feature3_weight[0].size();
                if (weight_data.size() >= expected_size) {
                    for (size_t i = 0; i < feature3_weight.size(); ++i) {
                        for (size_t j = 0; j < feature3_weight[i].size(); ++j) {
                            feature3_weight[i][j] = weight_data[i * feature3_weight[i].size() + j];
                        }
                    }
                } else {
                    std::cerr << "Feature3权重数据大小不匹配: 期望 " << expected_size << ", 实际 " << weight_data.size() << std::endl;
                }
            }
            if (layer.find("bias") != layer.end()) {
                auto& bias_data = layer["bias"];
                for (size_t i = 0; i < feature3_bias.size() && i < bias_data.size(); ++i) {
                    feature3_bias[i] = bias_data[i];
                }
            }
        }
        
        // 加载GRU权重
        if (weights.find("gru") != weights.end()) {
            auto& layer = weights["gru"];
            
            // 尝试不同的键名
            std::string weight_ih_key = "weight_ih";
            if (layer.find("weight_ih_l0") != layer.end()) {
                weight_ih_key = "weight_ih_l0";
            }
            
            if (layer.find(weight_ih_key) != layer.end()) {
                auto& weight_data = layer[weight_ih_key];
                
                size_t expected_size = gru_weight_ih.size() * gru_weight_ih[0].size();
                if (weight_data.size() >= expected_size) {
                    for (size_t i = 0; i < gru_weight_ih.size(); ++i) {
                        for (size_t j = 0; j < gru_weight_ih[i].size(); ++j) {
                            gru_weight_ih[i][j] = weight_data[i * gru_weight_ih[i].size() + j];
                        }
                    }
                } else {
                    std::cerr << "GRU " << weight_ih_key << " 数据大小不匹配: 期望 " << expected_size << ", 实际 " << weight_data.size() << std::endl;
                }
            }
            
            // 尝试不同的键名
            std::string weight_hh_key = "weight_hh";
            if (layer.find("weight_hh_l0") != layer.end()) {
                weight_hh_key = "weight_hh_l0";
            }
            
            if (layer.find(weight_hh_key) != layer.end()) {
                auto& weight_data = layer[weight_hh_key];
                
                size_t expected_size = gru_weight_hh.size() * gru_weight_hh[0].size();
                if (weight_data.size() >= expected_size) {
                    for (size_t i = 0; i < gru_weight_hh.size(); ++i) {
                        for (size_t j = 0; j < gru_weight_hh[i].size(); ++j) {
                            gru_weight_hh[i][j] = weight_data[i * gru_weight_hh[i].size() + j];
                        }
                    }
                } else {
                    std::cerr << "GRU " << weight_hh_key << " 数据大小不匹配: 期望 " << expected_size << ", 实际 " << weight_data.size() << std::endl;
                }
            }
            
            // 尝试不同的键名
            std::string bias_ih_key = "bias_ih";
            if (layer.find("bias_ih_l0") != layer.end()) {
                bias_ih_key = "bias_ih_l0";
            }
            
            if (layer.find(bias_ih_key) != layer.end()) {
                auto& bias_data = layer[bias_ih_key];
                for (size_t i = 0; i < gru_bias_ih.size() && i < bias_data.size(); ++i) {
                    gru_bias_ih[i] = bias_data[i];
                }
            }
            
            // 尝试不同的键名
            std::string bias_hh_key = "bias_hh";
            if (layer.find("bias_hh_l0") != layer.end()) {
                bias_hh_key = "bias_hh_l0";
            }
            
            if (layer.find(bias_hh_key) != layer.end()) {
                auto& bias_data = layer[bias_hh_key];
                for (size_t i = 0; i < gru_bias_hh.size() && i < bias_data.size(); ++i) {
                    gru_bias_hh[i] = bias_data[i];
                }
            }
        }
        
        // 加载输出层权重
        if (weights.find("pred1") != weights.end()) {
            auto& layer = weights["pred1"];
            if (layer.find("weight") != layer.end()) {
                auto& weight_data = layer["weight"];
                size_t expected_size = pred1_weight.size() * pred1_weight[0].size();
                if (weight_data.size() >= expected_size) {
                    for (size_t i = 0; i < pred1_weight.size(); ++i) {
                        for (size_t j = 0; j < pred1_weight[i].size(); ++j) {
                            pred1_weight[i][j] = weight_data[i * pred1_weight[i].size() + j];
                        }
                    }
                } else {
                    std::cerr << "Pred1权重数据大小不匹配: 期望 " << expected_size << ", 实际 " << weight_data.size() << std::endl;
                }
            }
            if (layer.find("bias") != layer.end()) {
                auto& bias_data = layer["bias"];
                for (size_t i = 0; i < pred1_bias.size() && i < bias_data.size(); ++i) {
                    pred1_bias[i] = bias_data[i];
                }
            }
        }
        
        if (weights.find("pred2") != weights.end()) {
            auto& layer = weights["pred2"];
            if (layer.find("weight") != layer.end()) {
                auto& weight_data = layer["weight"];
                size_t expected_size = pred2_weight.size() * pred2_weight[0].size();
                if (weight_data.size() >= expected_size) {
                    for (size_t i = 0; i < pred2_weight.size(); ++i) {
                        for (size_t j = 0; j < pred2_weight[i].size(); ++j) {
                            pred2_weight[i][j] = weight_data[i * pred2_weight[i].size() + j];
                        }
                    }
                } else {
                    std::cerr << "Pred2权重数据大小不匹配: 期望 " << expected_size << ", 实际 " << weight_data.size() << std::endl;
                }
            }
            if (layer.find("bias") != layer.end()) {
                auto& bias_data = layer["bias"];
                for (size_t i = 0; i < pred2_bias.size() && i < bias_data.size(); ++i) {
                    pred2_bias[i] = bias_data[i];
                }
            }
        }
        
        // 输出所有层权重和偏置的前5个数值
        
        // Feature1
        for (int i = 0; i < std::min(5, (int)feature1_weight.size() * (int)feature1_weight[0].size()); ++i) {
        }
        for (int i = 0; i < std::min(5, (int)feature1_bias.size()); ++i) {
        }
        
        // Feature2
        for (int i = 0; i < std::min(5, (int)feature2_weight.size() * (int)feature2_weight[0].size()); ++i) {
        }
        for (int i = 0; i < std::min(5, (int)feature2_bias.size()); ++i) {
        }
        
        // Feature3
        for (int i = 0; i < std::min(5, (int)feature3_weight.size() * (int)feature3_weight[0].size()); ++i) {
        }
        for (int i = 0; i < std::min(5, (int)feature3_bias.size()); ++i) {
        }
        
        // GRU
        for (int i = 0; i < std::min(5, (int)gru_weight_ih.size() * (int)gru_weight_ih[0].size()); ++i) {
        }
        for (int i = 0; i < std::min(5, (int)gru_weight_hh.size() * (int)gru_weight_hh[0].size()); ++i) {
        }
        for (int i = 0; i < std::min(5, (int)gru_bias_ih.size()); ++i) {
        }
        for (int i = 0; i < std::min(5, (int)gru_bias_hh.size()); ++i) {
        }
        
        // Pred1
        for (int i = 0; i < std::min(5, (int)pred1_weight.size() * (int)pred1_weight[0].size()); ++i) {
        }
        for (int i = 0; i < std::min(5, (int)pred1_bias.size()); ++i) {
        }
        
        // Pred2
        for (int i = 0; i < std::min(5, (int)pred2_weight.size() * (int)pred2_weight[0].size()); ++i) {
        }
        for (int i = 0; i < std::min(5, (int)pred2_bias.size()); ++i) {
        }
        
        return true;
    }
};

#endif // SIMPLE_RNN_MODEL_H
