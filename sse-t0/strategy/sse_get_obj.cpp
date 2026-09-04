#include "../../src/t0-main/strategy/StrategyBase.h"
#include "../../src/t0-main/json.hpp"
#include "sse_config_guard.h"
#include "IControlCenter.h"

#include <fstream>
#include <iostream>
#include <ctime>

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

// Keep deployment settings in the live file and load only the date-specific
// contract (ins_params/global_params/trading_day) into memory at startup.
bool merge_daily_config(json& live) {
    json::const_iterator ref = live.find("daily_config_path");
    if (ref == live.end()) return true;  // legacy single-file config
    if (!ref->is_string() || ref->get<std::string>().empty()) {
        std::cerr << "[sse_get_obj] daily_config_path must be a non-empty string\n";
        return false;
    }
    std::string daily_path = ref->get<std::string>();
    const std::string token = "${TRADING_DATE}";
    const std::string::size_type pos = daily_path.find(token);
    if (pos != std::string::npos) {
        std::time_t now = std::time(0);
        std::tm* local = std::localtime(&now);
        char date[16] = {0};
        if (local == 0 || std::strftime(date, sizeof(date), "%Y%m%d", local) == 0)
            return false;
        daily_path.replace(pos, token.size(), date);
    }
    json daily;
    if (!file2json(daily_path, daily)) {
        std::cerr << "[sse_get_obj] failed to load daily config: "
                  << daily_path << '\n';
        return false;
    }
    const char* keys[] = {"trading_day", "static_data_source_date", "ins_params",
                          "global_params", "static_position"};
    for (std::size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        json::const_iterator item = daily.find(keys[i]);
        if (item != daily.end()) live[keys[i]] = *item;
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
    return "sse-strategy-v06-hybrid-td-routing-20260827";
}

IWCStrategy* get_obj(kungfu::yijinjing::IControlCenter* cc, const std::string& cfg_name) {
    const std::string path = cfg_name.empty() ? "config_sse.json" : cfg_name;
    json config;
    if (!file2json(path, config)) return nullptr;
    if (!merge_daily_config(config)) return nullptr;
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
