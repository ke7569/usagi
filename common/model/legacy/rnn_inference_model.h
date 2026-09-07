#ifndef RNN_INFERENCE_MODEL_H
#define RNN_INFERENCE_MODEL_H

#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <cmath>
#include <random>
#include <memory>
#include "common/config/simple_json_parser.h"

class RNNInferenceModel {
private:
    // 模型参数
    int n_feature;
    int n_rnn_neuron;
    std::string dense_activation;
    std::string rnn_activation;
    std::string rnn_initializer;
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
    
    // 辅助函数
    float relu(float x) { return x > 0 ? x : 0; }
    float tanh_activation(float x) { return std::tanh(x); }
    float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }
    
    float get_activation(const std::string& activation_name, float x) {
        if (activation_name == "relu") return relu(x);
        else if (activation_name == "tanh") return tanh_activation(x);
        else if (activation_name == "sigmoid") return sigmoid(x);
        else return relu(x);
    }
    
    // 线性层前向传播
    std::vector<float> linear_forward(const std::vector<float>& input, 
                                    const std::vector<std::vector<float>>& weight,
                                    const std::vector<float>& bias) {
        std::vector<float> output(weight.size(), 0.0f);
        for (size_t i = 0; i < weight.size(); ++i) {
            for (size_t j = 0; j < input.size(); ++j) {
                output[i] += weight[i][j] * input[j];
            }
            output[i] += bias[i];
        }
        return output;
    }
    
    // GRU 前向传播
    std::vector<float> gru_forward(const std::vector<float>& input) {
        int hidden_size = n_rnn_neuron;
        std::vector<float> new_hidden(hidden_size, 0.0f);
        
        // 计算门控
        std::vector<float> reset_gate(hidden_size), update_gate(hidden_size), new_gate(hidden_size);
        
        // 重置门
        for (int i = 0; i < hidden_size; ++i) {
            float sum = 0.0f;
            for (int j = 0; j < input.size(); ++j) {
                sum += gru_weight_ih[i][j] * input[j];
            }
            for (int j = 0; j < hidden_size; ++j) {
                sum += gru_weight_hh[i][j] * hidden_state[j];
            }
            reset_gate[i] = sigmoid(sum + gru_bias_ih[i] + gru_bias_hh[i]);
        }
        
        // 更新门
        for (int i = 0; i < hidden_size; ++i) {
            float sum = 0.0f;
            for (int j = 0; j < input.size(); ++j) {
                sum += gru_weight_ih[hidden_size + i][j] * input[j];
            }
            for (int j = 0; j < hidden_size; ++j) {
                sum += gru_weight_hh[hidden_size + i][j] * hidden_state[j];
            }
            update_gate[i] = sigmoid(sum + gru_bias_ih[hidden_size + i] + gru_bias_hh[hidden_size + i]);
        }
        
        // 新候选隐藏状态
        for (int i = 0; i < hidden_size; ++i) {
            float sum = 0.0f;
            for (int j = 0; j < input.size(); ++j) {
                sum += gru_weight_ih[2 * hidden_size + i][j] * input[j];
            }
            for (int j = 0; j < hidden_size; ++j) {
                sum += gru_weight_hh[2 * hidden_size + i][j] * (reset_gate[j] * hidden_state[j]);
            }
            new_gate[i] = tanh_activation(sum + gru_bias_ih[2 * hidden_size + i] + gru_bias_hh[2 * hidden_size + i]);
        }
        
        // 更新隐藏状态
        for (int i = 0; i < hidden_size; ++i) {
            new_hidden[i] = (1.0f - update_gate[i]) * new_gate[i] + update_gate[i] * hidden_state[i];
        }
        
        hidden_state = new_hidden;
        return new_hidden;
    }
    
    // 随机初始化权重
    void initialize_weights() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(-0.1, 0.1);
        
        // 初始化特征提取层
        for (auto& row : feature1_weight) {
            for (auto& val : row) val = dis(gen);
        }
        for (auto& val : feature1_bias) val = 0.0f;
        
        for (auto& row : feature2_weight) {
            for (auto& val : row) val = dis(gen);
        }
        for (auto& val : feature2_bias) val = 0.0f;
        
        for (auto& row : feature3_weight) {
            for (auto& val : row) val = dis(gen);
        }
        for (auto& val : feature3_bias) val = 0.0f;
        
        // 初始化 GRU 层
        for (auto& row : gru_weight_ih) {
            for (auto& val : row) val = dis(gen);
        }
        for (auto& row : gru_weight_hh) {
            for (auto& val : row) val = dis(gen);
        }
        for (auto& val : gru_bias_ih) val = 0.0f;
        for (auto& val : gru_bias_hh) val = 0.0f;
        
        // 初始化输出层
        for (auto& row : pred1_weight) {
            for (auto& val : row) val = dis(gen);
        }
        for (auto& val : pred1_bias) val = 0.0f;
        
        for (auto& row : pred2_weight) {
            for (auto& val : row) val = dis(gen);
        }
        for (auto& val : pred2_bias) val = 0.0f;
    }

public:
    RNNInferenceModel(int n_feat = 30, int n_rnn = 64, 
                     const std::string& dense_act = "relu",
                     const std::string& rnn_act = "tanh",
                     const std::string& rnn_init = "orthogonal",
                     float dropout = 0.2f)
        : n_feature(n_feat), n_rnn_neuron(n_rnn), 
          dense_activation(dense_act), rnn_activation(rnn_act),
          rnn_initializer(rnn_init), dropout_rate(dropout) {
        
        // 初始化权重矩阵
        feature1_weight.resize(n_rnn_neuron * 8, std::vector<float>(n_feature));
        feature1_bias.resize(n_rnn_neuron * 8);
        
        feature2_weight.resize(n_rnn_neuron * 4, std::vector<float>(n_rnn_neuron * 8));
        feature2_bias.resize(n_rnn_neuron * 4);
        
        feature3_weight.resize(n_rnn_neuron * 2, std::vector<float>(n_rnn_neuron * 4));
        feature3_bias.resize(n_rnn_neuron * 2);
        
        gru_weight_ih.resize(n_rnn_neuron * 3, std::vector<float>(n_rnn_neuron * 2));
        gru_weight_hh.resize(n_rnn_neuron * 3, std::vector<float>(n_rnn_neuron));
        gru_bias_ih.resize(n_rnn_neuron * 3);
        gru_bias_hh.resize(n_rnn_neuron * 3);
        
        pred1_weight.resize(8, std::vector<float>(n_rnn_neuron));
        pred1_bias.resize(8);
        
        pred2_weight.resize(1, std::vector<float>(8));
        pred2_bias.resize(1);
        
        hidden_state.resize(n_rnn_neuron, 0.0f);
        
        initialize_weights();
    }
    
    // 前向传播
    std::vector<float> forward(const std::vector<std::vector<float>>& input) {
        int seq_len = input.size();
        std::vector<float> outputs;
        
        // 重置隐藏状态
        std::fill(hidden_state.begin(), hidden_state.end(), 0.0f);
        
        for (int t = 0; t < seq_len; ++t) {
            // 特征提取
            auto x = linear_forward(input[t], feature1_weight, feature1_bias);
            for (auto& val : x) val = get_activation(dense_activation, val);
            
            x = linear_forward(x, feature2_weight, feature2_bias);
            for (auto& val : x) val = get_activation(dense_activation, val);
            
            x = linear_forward(x, feature3_weight, feature3_bias);
            for (auto& val : x) val = get_activation(dense_activation, val);
            
            // GRU 层
            auto gru_out = gru_forward(x);
            
            // 输出层
            auto pred1_out = linear_forward(gru_out, pred1_weight, pred1_bias);
            for (auto& val : pred1_out) val = get_activation(dense_activation, val);
            
            auto pred2_out = linear_forward(pred1_out, pred2_weight, pred2_bias);
            outputs.push_back(pred2_out[0]);
        }
        
        return outputs;
    }
    
    // 单步预测方法（维护隐状态）
    float predict_single_step(const std::vector<float>& input) {
        if (hidden_state_.empty()) {
            hidden_state_.resize(n_rnn_neuron, 0.0f);
        }
        
        // 特征提取
        std::vector<float> f1_out = feature1.forward(input);
        for (float& val : f1_out) val = dense_act_fn(val);
        std::vector<float> d1_out = dropout_forward(f1_out);

        std::vector<float> f2_out = feature2.forward(d1_out);
        for (float& val : f2_out) val = dense_act_fn(val);
        std::vector<float> d2_out = dropout_forward(f2_out);

        std::vector<float> f3_out = feature3.forward(d2_out);
        for (float& val : f3_out) val = dense_act_fn(val);
        std::vector<float> d3_out = dropout_forward(f3_out);

        // GRU层（更新隐状态）
        std::vector<float> gru_output = gru.forward(d3_out, hidden_state_);

        // 输出层
        std::vector<float> p1_out = pred1.forward(gru_output);
        for (float& val : p1_out) val = dense_act_fn(val);
        std::vector<float> d4_out = dropout_forward(p1_out);
        
        std::vector<float> p2_out = pred2.forward(d4_out);
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
        if (!config.empty()) {
                      << ", n_rnn_neuron=" << config["n_rnn_neuron"] << std::endl;
        }
        
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
                for (size_t i = 0; i < feature1_weight.size() && i < weight_data.size(); ++i) {
                    for (size_t j = 0; j < feature1_weight[i].size() && j < weight_data.size(); ++j) {
                        feature1_weight[i][j] = weight_data[j];
                    }
                }
            }
            if (layer.find("bias") != layer.end()) {
                auto& bias_data = layer["bias"];
                for (size_t i = 0; i < feature1_bias.size() && i < bias_data.size(); ++i) {
                    feature1_bias[i] = bias_data[i];
                }
            }
        }
        
        if (weights.find("feature2") != weights.end()) {
            auto& layer = weights["feature2"];
            if (layer.find("weight") != layer.end()) {
                auto& weight_data = layer["weight"];
                for (size_t i = 0; i < feature2_weight.size() && i < weight_data.size(); ++i) {
                    for (size_t j = 0; j < feature2_weight[i].size() && j < weight_data.size(); ++j) {
                        feature2_weight[i][j] = weight_data[j];
                    }
                }
            }
            if (layer.find("bias") != layer.end()) {
                auto& bias_data = layer["bias"];
                for (size_t i = 0; i < feature2_bias.size() && i < bias_data.size(); ++i) {
                    feature2_bias[i] = bias_data[i];
                }
            }
        }
        
        if (weights.find("feature3") != weights.end()) {
            auto& layer = weights["feature3"];
            if (layer.find("weight") != layer.end()) {
                auto& weight_data = layer["weight"];
                for (size_t i = 0; i < feature3_weight.size() && i < weight_data.size(); ++i) {
                    for (size_t j = 0; j < feature3_weight[i].size() && j < weight_data.size(); ++j) {
                        feature3_weight[i][j] = weight_data[j];
                    }
                }
            }
            if (layer.find("bias") != layer.end()) {
                auto& bias_data = layer["bias"];
                for (size_t i = 0; i < feature3_bias.size() && i < bias_data.size(); ++i) {
                    feature3_bias[i] = bias_data[i];
                }
            }
        }
        
        // 加载 GRU 权重
        if (weights.find("gru") != weights.end()) {
            auto& layer = weights["gru"];
            if (layer.find("weight_ih") != layer.end()) {
                auto& weight_data = layer["weight_ih"];
                for (size_t i = 0; i < gru_weight_ih.size() && i < weight_data.size(); ++i) {
                    for (size_t j = 0; j < gru_weight_ih[i].size() && j < weight_data.size(); ++j) {
                        gru_weight_ih[i][j] = weight_data[j];
                    }
                }
            }
            if (layer.find("weight_hh") != layer.end()) {
                auto& weight_data = layer["weight_hh"];
                for (size_t i = 0; i < gru_weight_hh.size() && i < weight_data.size(); ++i) {
                    for (size_t j = 0; j < gru_weight_hh[i].size() && j < weight_data.size(); ++j) {
                        gru_weight_hh[i][j] = weight_data[j];
                    }
                }
            }
            if (layer.find("bias_ih") != layer.end()) {
                auto& bias_data = layer["bias_ih"];
                for (size_t i = 0; i < gru_bias_ih.size() && i < bias_data.size(); ++i) {
                    gru_bias_ih[i] = bias_data[i];
                }
            }
            if (layer.find("bias_hh") != layer.end()) {
                auto& bias_data = layer["bias_hh"];
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
                for (size_t i = 0; i < pred1_weight.size() && i < weight_data.size(); ++i) {
                    for (size_t j = 0; j < pred1_weight[i].size() && j < weight_data.size(); ++j) {
                        pred1_weight[i][j] = weight_data[j];
                    }
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
                for (size_t i = 0; i < pred2_weight.size() && i < weight_data.size(); ++i) {
                    for (size_t j = 0; j < pred2_weight[i].size() && j < weight_data.size(); ++j) {
                        pred2_weight[i][j] = weight_data[j];
                    }
                }
            }
            if (layer.find("bias") != layer.end()) {
                auto& bias_data = layer["bias"];
                for (size_t i = 0; i < pred2_bias.size() && i < bias_data.size(); ++i) {
                    pred2_bias[i] = bias_data[i];
                }
            }
        }
        
        return true;
    }
};

#endif // RNN_INFERENCE_MODEL_H
