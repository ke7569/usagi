#include "../../src/t0-main/strategy/StrategyBase.h"
#include "../../src/t0-main/json.hpp"
#include "sse_config_guard.h"
#include "IControlCenter.h"

#include <fstream>
#include <iostream>

using nlohmann::json;

namespace {
bool file2json(const std::string& path, json& out) {
    std::ifstream file(path.c_str());
    if (!file.is_open()) {
        std::cerr << "[sse_get_obj] failed to open config: " << path << '\n';
        return false;
    }
    try { file >> out; }
    catch (const std::exception& e) {
        std::cerr << "[sse_get_obj] parse error: " << e.what() << '\n';
        return false;
    }
    return true;
}
}

#define EXPORT_FLAG __attribute__((__visibility__("default")))
extern "C" {
EXPORT_FLAG IWCStrategy* get_obj(kungfu::yijinjing::IControlCenter*, const std::string&);
EXPORT_FLAG const char* sse_strategy_build_id();
}

const char* sse_strategy_build_id() {
    return "sse-strategy-v05-td-routing-20260820";
}

IWCStrategy* get_obj(kungfu::yijinjing::IControlCenter* cc, const std::string& cfg_name) {
    const std::string path = cfg_name.empty() ? "config_sse.json" : cfg_name;
    json config;
    if (!file2json(path, config)) return nullptr;
    std::string error;
    if (!sse_strategy_library::validate_config(config, &error)) {
        std::cerr << "[sse_get_obj] " << error << '\n';
        return nullptr;
    }
    std::string strategy_name = "sse_strategy";
    if (config.find("strategy_name") != config.end())
        strategy_name = config["strategy_name"].get<std::string>();
    else if (config.find("name") != config.end())
        strategy_name = config["name"].get<std::string>();
    try {
        StrategyBase* strategy = new StrategyBase(strategy_name, config);
        strategy->set_cc(cc);
        if (cc != nullptr) {
            try { (void)cc->get_rid_pair(strategy_name); }
            catch (const std::exception& e) {
                std::cerr << "[sse_get_obj] get_rid_pair threw: " << e.what() << '\n';
            }
            cc->set_str(reinterpret_cast<void*>(strategy));
        }
        strategy->init();
        strategy->start();
        std::cerr << "[sse_get_obj] build_id=" << sse_strategy_build_id()
                  << " strategy_started=1" << '\n';
        return strategy;
    } catch (const std::exception& e) {
        std::cerr << "[sse_get_obj] strategy construction failed: " << e.what() << '\n';
        return nullptr;
    }
}
