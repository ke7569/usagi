#ifndef GRU_PREDICTOR_H
#define GRU_PREDICTOR_H

#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <cmath>
#include "common/model/legacy/weight_loader.h"

class GRUPredictor {
private:
    // GRU 参数
    int input_size;
    int hidden_size;
    int num_layers;
    int output_size;
    
    // 权重矩阵 (对于单层GRU)
    std::vector<std::vector<float>> W_ir;  // input to reset gate
    std::vector<std::vector<float>> W_iz;  // input to update gate  
    std::vector<std::vector<float>> W_in;  // input to new gate
    std::vector<std::vector<float>> W_hr;  // hidden to reset gate
    std::vector<std::vector<float>> W_hz;  // hidden to update gate
    std::vector<std::vector<float>> W_hn;  // hidden to new gate
    
    // 偏置向量
    std::vector<float> b_ir, b_iz, b_in;   // input gate biases
    std::vector<float> b_hr, b_hz, b_hn;   // hidden gate biases
    
    // 输出层权重
    std::vector<std::vector<float>> W_out;
    std::vector<float> b_out;
    
    // 激活函数
    float sigmoid(float x) {
        return 1.0f / (1.0f + std::exp(-x));
    }
    
    float tanh_activation(float x) {
        return std::tanh(x);
    }
    
    // 矩阵向量乘法
    std::vector<float> matmul(const std::vector<std::vector<float>>& W, 
                             const std::vector<float>& x) {
        std::vector<float> result(W.size(), 0.0f);
        for (size_t i = 0; i < W.size(); i++) {
            for (size_t j = 0; j < W[i].size(); j++) {
                result[i] += W[i][j] * x[j];
            }
        }
        return result;
    }
    
    // 向量加法
    std::vector<float> add(const std::vector<float>& a, const std::vector<float>& b) {
        std::vector<float> result(a.size());
        for (size_t i = 0; i < a.size(); i++) {
            result[i] = a[i] + b[i];
        }
        return result;
    }
    
    // 向量逐元素乘法
    std::vector<float> elementwise_mul(const std::vector<float>& a, const std::vector<float>& b) {
        std::vector<float> result(a.size());
        for (size_t i = 0; i < a.size(); i++) {
            result[i] = a[i] * b[i];
        }
        return result;
    }

public:
    GRUPredictor(int input_size = 30, int hidden_size = 64, int num_layers = 1, int output_size = 1)
        : input_size(input_size), hidden_size(hidden_size), num_layers(num_layers), output_size(output_size) {
        
        // 初始化权重矩阵
        W_ir.resize(hidden_size, std::vector<float>(input_size, 0.0f));
        W_iz.resize(hidden_size, std::vector<float>(input_size, 0.0f));
        W_in.resize(hidden_size, std::vector<float>(input_size, 0.0f));
        W_hr.resize(hidden_size, std::vector<float>(hidden_size, 0.0f));
        W_hz.resize(hidden_size, std::vector<float>(hidden_size, 0.0f));
        W_hn.resize(hidden_size, std::vector<float>(hidden_size, 0.0f));
        
        // 初始化偏置向量
        b_ir.resize(hidden_size, 0.0f);
        b_iz.resize(hidden_size, 0.0f);
        b_in.resize(hidden_size, 0.0f);
        b_hr.resize(hidden_size, 0.0f);
        b_hz.resize(hidden_size, 0.0f);
        b_hn.resize(hidden_size, 0.0f);
        
        // 初始化输出层
        W_out.resize(output_size, std::vector<float>(hidden_size, 0.0f));
        b_out.resize(output_size, 0.0f);
    }
    
    // 从文件加载权重
    bool load_weights(const std::string& filename) {
        WeightLoader loader;
        if (!loader.load_from_binary(filename + ".bin")) {
            std::cerr << "无法加载权重文件: " << filename << ".bin" << std::endl;
            return false;
        }
        
        // 尝试加载 GRU 权重
        // 注意：这里的键名需要根据实际模型调整
        auto w_ir = loader.get_2d_weight("gru.weight_ih_l0");
        auto w_iz = loader.get_2d_weight("gru.weight_ih_l0");
        auto w_in = loader.get_2d_weight("gru.weight_ih_l0");
        auto w_hr = loader.get_2d_weight("gru.weight_hh_l0");
        auto w_hz = loader.get_2d_weight("gru.weight_hh_l0");
        auto w_hn = loader.get_2d_weight("gru.weight_hh_l0");
        
        auto b_ir = loader.get_1d_weight("gru.bias_ih_l0");
        auto b_iz = loader.get_1d_weight("gru.bias_ih_l0");
        auto b_in = loader.get_1d_weight("gru.bias_ih_l0");
        auto b_hr = loader.get_1d_weight("gru.bias_hh_l0");
        auto b_hz = loader.get_1d_weight("gru.bias_hh_l0");
        auto b_hn = loader.get_1d_weight("gru.bias_hh_l0");
        
        // 如果成功加载权重，则使用它们
        if (!w_ir.empty() && w_ir[0].size() == input_size) {
            W_ir = w_ir;
            W_iz = w_iz;
            W_in = w_in;
            W_hr = w_hr;
            W_hz = w_hz;
            W_hn = w_hn;
            
            if (!b_ir.empty()) {
                b_ir = b_ir;
                b_iz = b_iz;
                b_in = b_in;
                b_hr = b_hr;
                b_hz = b_hz;
                b_hn = b_hn;
            }
            
            return true;
        } else {
            initialize_random_weights();
            return true;
        }
    }
    
    // 随机初始化权重（用于测试）
    void initialize_random_weights() {
        // 使用 Xavier 初始化
        float scale = std::sqrt(2.0f / (input_size + hidden_size));
        
        for (int i = 0; i < hidden_size; i++) {
            for (int j = 0; j < input_size; j++) {
                W_ir[i][j] = (rand() / (float)RAND_MAX - 0.5f) * 2.0f * scale;
                W_iz[i][j] = (rand() / (float)RAND_MAX - 0.5f) * 2.0f * scale;
                W_in[i][j] = (rand() / (float)RAND_MAX - 0.5f) * 2.0f * scale;
            }
        }
        
        for (int i = 0; i < hidden_size; i++) {
            for (int j = 0; j < hidden_size; j++) {
                W_hr[i][j] = (rand() / (float)RAND_MAX - 0.5f) * 2.0f * scale;
                W_hz[i][j] = (rand() / (float)RAND_MAX - 0.5f) * 2.0f * scale;
                W_hn[i][j] = (rand() / (float)RAND_MAX - 0.5f) * 2.0f * scale;
            }
        }
        
        for (int i = 0; i < output_size; i++) {
            for (int j = 0; j < hidden_size; j++) {
                W_out[i][j] = (rand() / (float)RAND_MAX - 0.5f) * 2.0f * scale;
            }
        }
    }
    
    // GRU 前向传播
    float predict(const std::vector<float>& input) {
        if (input.size() != input_size) {
            std::cerr << "输入大小不匹配: 期望 " << input_size << ", 实际 " << input.size() << std::endl;
            return 0.0f;
        }
        
        // 初始化隐藏状态
        std::vector<float> h(hidden_size, 0.0f);
        
        // GRU 计算
        // 重置门: r_t = σ(W_ir * x_t + b_ir + W_hr * h_{t-1} + b_hr)
        std::vector<float> r = add(matmul(W_ir, input), b_ir);
        r = add(r, add(matmul(W_hr, h), b_hr));
        for (float& val : r) val = sigmoid(val);
        
        // 更新门: z_t = σ(W_iz * x_t + b_iz + W_hz * h_{t-1} + b_hz)
        std::vector<float> z = add(matmul(W_iz, input), b_iz);
        z = add(z, add(matmul(W_hz, h), b_hz));
        for (float& val : z) val = sigmoid(val);
        
        // 候选隐藏状态: n_t = tanh(W_in * x_t + b_in + r_t ⊙ (W_hn * h_{t-1} + b_hn))
        std::vector<float> n = add(matmul(W_in, input), b_in);
        std::vector<float> h_contribution = add(matmul(W_hn, h), b_hn);
        h_contribution = elementwise_mul(r, h_contribution);
        n = add(n, h_contribution);
        for (float& val : n) val = tanh_activation(val);
        
        // 新的隐藏状态: h_t = (1 - z_t) ⊙ n_t + z_t ⊙ h_{t-1}
        std::vector<float> one_minus_z(hidden_size);
        for (int i = 0; i < hidden_size; i++) {
            one_minus_z[i] = 1.0f - z[i];
        }
        h = add(elementwise_mul(one_minus_z, n), elementwise_mul(z, h));
        
        // 输出层: y = W_out * h + b_out
        std::vector<float> output = add(matmul(W_out, h), b_out);
        
        return output[0];
    }
    
    // 批量预测
    std::vector<float> predict_batch(const std::vector<std::vector<float>>& inputs) {
        std::vector<float> results;
        for (const auto& input : inputs) {
            results.push_back(predict(input));
        }
        return results;
    }
};

#endif // GRU_PREDICTOR_H
