#ifndef WEIGHT_LOADER_H
#define WEIGHT_LOADER_H

#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <cstring>

class WeightLoader {
private:
    std::map<std::string, std::vector<std::vector<float>>> weights_2d;
    std::map<std::string, std::vector<float>> weights_1d;
    
public:
    bool load_from_binary(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "无法打开权重文件: " << filename << std::endl;
            return false;
        }
        
        while (!file.eof()) {
            // 读取键长度
            uint32_t key_len;
            file.read(reinterpret_cast<char*>(&key_len), sizeof(key_len));
            if (file.eof()) break;
            
            // 读取键
            std::string key(key_len, '\0');
            file.read(&key[0], key_len);
            
            // 读取维度数
            uint32_t dims;
            file.read(reinterpret_cast<char*>(&dims), sizeof(dims));
            
            if (dims == 1) {
                // 1D 数组
                uint32_t len;
                file.read(reinterpret_cast<char*>(&len), sizeof(len));
                
                std::vector<float> data(len);
                file.read(reinterpret_cast<char*>(data.data()), len * sizeof(float));
                weights_1d[key] = data;
                
            } else if (dims == 2) {
                // 2D 数组
                uint32_t rows, cols;
                file.read(reinterpret_cast<char*>(&rows), sizeof(rows));
                file.read(reinterpret_cast<char*>(&cols), sizeof(cols));
                
                std::vector<std::vector<float>> data(rows, std::vector<float>(cols));
                for (uint32_t i = 0; i < rows; i++) {
                    file.read(reinterpret_cast<char*>(data[i].data()), cols * sizeof(float));
                }
                weights_2d[key] = data;
            }
        }
        
        file.close();
        return true;
    }
    
    bool load_from_json(const std::string& filename) {
        // 简单的 JSON 解析器（仅支持基本格式）
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "无法打开 JSON 文件: " << filename << std::endl;
            return false;
        }
        
        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        file.close();
        
        // 这里需要实现 JSON 解析
        // 为了简化，我们假设权重已经通过其他方式加载
        return true;
    }
    
    std::vector<std::vector<float>> get_2d_weight(const std::string& key) {
        auto it = weights_2d.find(key);
        if (it != weights_2d.end()) {
            return it->second;
        }
        return std::vector<std::vector<float>>();
    }
    
    std::vector<float> get_1d_weight(const std::string& key) {
        auto it = weights_1d.find(key);
        if (it != weights_1d.end()) {
            return it->second;
        }
        return std::vector<float>();
    }

    void print_loaded_weights() {
    }
};

#endif // WEIGHT_LOADER_H
