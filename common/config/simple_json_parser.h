#ifndef SIMPLE_JSON_PARSER_H
#define SIMPLE_JSON_PARSER_H

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>

class SimpleJsonParser {
private:
    std::string json_content;
    size_t pos;
    
    void skip_whitespace() {
        while (pos < json_content.length() && 
               (json_content[pos] == ' ' || json_content[pos] == '\n' || 
                json_content[pos] == '\r' || json_content[pos] == '\t')) {
            pos++;
        }
    }
    
    std::string parse_string() {
        skip_whitespace();
        if (pos >= json_content.length() || json_content[pos] != '"') {
            return "";
        }
        pos++; // skip opening quote
        std::string result;
        while (pos < json_content.length() && json_content[pos] != '"') {
            result += json_content[pos];
            pos++;
        }
        if (pos < json_content.length()) pos++; // skip closing quote
        return result;
    }
    
    double parse_number() {
        skip_whitespace();
        std::string num_str;
        bool has_digit = false;
        
        // Handle negative sign
        if (pos < json_content.length() && json_content[pos] == '-') {
            num_str += json_content[pos];
            pos++;
        }
        
        // Parse digits and decimal point
        while (pos < json_content.length()) {
            char c = json_content[pos];
            if (std::isdigit(c)) {
                num_str += c;
                has_digit = true;
                pos++;
            } else if (c == '.' && has_digit) {
                num_str += c;
                pos++;
            } else if ((c == 'e' || c == 'E') && has_digit) {
                num_str += c;
                pos++;
                // Handle exponent sign
                if (pos < json_content.length() && 
                    (json_content[pos] == '+' || json_content[pos] == '-')) {
                    num_str += json_content[pos];
                    pos++;
                }
            } else {
                break;
            }
        }
        
        if (num_str.empty() || !has_digit) {
            return 0.0;
        }
        
        try {
            return std::stod(num_str);
        } catch (const std::exception& e) {
            std::cerr << "解析数字失败: " << num_str << std::endl;
            return 0.0;
        }
    }
    
    std::vector<double> parse_array() {
        skip_whitespace();
        if (pos >= json_content.length() || json_content[pos] != '[') {
            return {};
        }
        pos++; // skip opening bracket
        std::vector<double> result;
        
        while (pos < json_content.length()) {
            skip_whitespace();
            if (json_content[pos] == ']') {
                pos++;
                break;
            }
            if (json_content[pos] == ',') {
                pos++;
                skip_whitespace();
                continue;
            }
            
            // Check if this is a nested array (skip it for now)
            if (json_content[pos] == '[') {
                // Skip nested array
                int bracket_count = 1;
                pos++;
                while (pos < json_content.length() && bracket_count > 0) {
                    if (json_content[pos] == '[') bracket_count++;
                    else if (json_content[pos] == ']') bracket_count--;
                    pos++;
                }
                continue;
            }
            
            result.push_back(parse_number());
        }
        return result;
    }
    
    std::map<std::string, std::vector<double>> parse_layer_weights() {
        skip_whitespace();
        if (pos >= json_content.length() || json_content[pos] != '{') {
            return {};
        }
        pos++; // skip opening brace
        std::map<std::string, std::vector<double>> result;
        
        while (pos < json_content.length()) {
            skip_whitespace();
            if (json_content[pos] == '}') {
                pos++;
                break;
            }
            if (json_content[pos] == ',') {
                pos++;
                continue;
            }
            
            std::string key = parse_string();
            skip_whitespace();
            if (pos < json_content.length() && json_content[pos] == ':') {
                pos++;
            }
            
            // Check if this is a 2D array (weight matrix)
            skip_whitespace();
            if (json_content[pos] == '[') {
                // Check if it's a 2D array by looking ahead
                size_t check_pos = pos + 1;
                bool is_2d = false;
                while (check_pos < json_content.length()) {
                    if (json_content[check_pos] == '[') {
                        is_2d = true;
                        break;
                    } else if (json_content[check_pos] == ']') {
                        break;
                    }
                    check_pos++;
                }
                
                if (is_2d) {
                    // Flatten 2D array to 1D
                    std::vector<double> flattened;
                    pos++; // skip opening bracket
                    
                    while (pos < json_content.length()) {
                        skip_whitespace();
                        if (json_content[pos] == ']') {
                            pos++;
                            break;
                        }
                        if (json_content[pos] == ',') {
                            pos++;
                            continue;
                        }
                        
                        // Parse inner array
                        if (json_content[pos] == '[') {
                            pos++; // skip opening bracket
                            while (pos < json_content.length()) {
                                skip_whitespace();
                                if (json_content[pos] == ']') {
                                    pos++;
                                    break;
                                }
                                if (json_content[pos] == ',') {
                                    pos++;
                                    continue;
                                }
                                flattened.push_back(parse_number());
                            }
                        }
                    }
                    result[key] = flattened;
                } else {
                    result[key] = parse_array();
                }
            }
        }
        return result;
    }
    
public:
    std::map<std::string, std::map<std::string, std::vector<double>>> parse_weights() {
        // Find the weights section
        size_t weights_pos = json_content.find("\"weights\"");
        if (weights_pos == std::string::npos) {
            std::cerr << "未找到 weights 部分" << std::endl;
            return {};
        }
        
        pos = weights_pos;
        while (pos < json_content.length() && json_content[pos] != ':') {
            pos++;
        }
        if (pos < json_content.length()) pos++; // skip colon
        
        skip_whitespace();
        if (pos >= json_content.length() || json_content[pos] != '{') {
            std::cerr << "weights 部分格式错误" << std::endl;
            return {};
        }
        pos++; // skip opening brace
        
        std::map<std::string, std::map<std::string, std::vector<double>>> result;
        
        while (pos < json_content.length()) {
            skip_whitespace();
            if (json_content[pos] == '}') {
                pos++;
                break;
            }
            if (json_content[pos] == ',') {
                pos++;
                continue;
            }
            
            std::string layer_name = parse_string();
            skip_whitespace();
            if (pos < json_content.length() && json_content[pos] == ':') {
                pos++;
            }
            
            result[layer_name] = parse_layer_weights();
        }
        return result;
    }
    bool load_from_file(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "无法打开文件: " << filename << std::endl;
            return false;
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        json_content = buffer.str();
        file.close();
        pos = 0;
        return true;
    }
    
    std::map<std::string, double> parse_model_config() {
        // Find the model_config section
        size_t config_pos = json_content.find("\"model_config\"");
        if (config_pos == std::string::npos) {
            std::cerr << "未找到 model_config 部分" << std::endl;
            return {};
        }
        
        pos = config_pos;
        while (pos < json_content.length() && json_content[pos] != ':') {
            pos++;
        }
        if (pos < json_content.length()) pos++; // skip colon
        
        skip_whitespace();
        if (pos >= json_content.length() || json_content[pos] != '{') {
            std::cerr << "model_config 部分格式错误" << std::endl;
            return {};
        }
        pos++; // skip opening brace
        
        std::map<std::string, double> result;
        
        while (pos < json_content.length()) {
            skip_whitespace();
            if (json_content[pos] == '}') {
                pos++;
                break;
            }
            if (json_content[pos] == ',') {
                pos++;
                continue;
            }
            
            std::string key = parse_string();
            skip_whitespace();
            if (pos < json_content.length() && json_content[pos] == ':') {
                pos++;
            }
            
            if (key == "dense_activation" || key == "rnn_activation") {
                // Skip string values for now
                parse_string();
            } else {
                result[key] = parse_number();
            }
        }
        return result;
    }
};

#endif // SIMPLE_JSON_PARSER_H
