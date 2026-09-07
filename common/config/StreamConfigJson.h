#ifndef T0_STREAM_CONFIG_JSON_H
#define T0_STREAM_CONFIG_JSON_H

#include "third_party/nlohmann/json.hpp"
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

inline nlohmann::json load_stream_json(const std::string& path) {
    typedef nlohmann::json Json;
    std::ifstream input(path.c_str());
    if (!input) throw std::runtime_error("cannot open config: " + path);
    std::vector<std::set<std::string> > keys;
    return Json::parse(input, [&](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::object_start) keys.push_back(std::set<std::string>());
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::runtime_error("duplicate config key: " + value.get<std::string>());
        return true;
    });
}

#endif
