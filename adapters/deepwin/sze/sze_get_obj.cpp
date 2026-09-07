#include "adapters/deepwin/strategy/StrategyBase.h"
#include "third_party/nlohmann/json.hpp"
#include "adapters/deepwin/sze/sze_config_guard.h"
#include "IControlCenter.h"

#include <fstream>
#include <iostream>

using nlohmann::json;

namespace {
bool file2json(const std::string& filename, json& out) {
    std::ifstream file(filename.c_str());
    if (!file.is_open()) {
        std::cerr << "[sze_get_obj] failed to open config: " << filename << '\n';
        return false;
    }
    try {
        file >> out;
    } catch (const std::exception& e) {
        std::cerr << "[sze_get_obj] parse error: " << e.what() << '\n';
        return false;
    }
    return true;
}
}

#define EXPORT_FLAG __attribute__((__visibility__("default")))

#ifdef __cplusplus
extern "C" {
#endif

EXPORT_FLAG IWCStrategy* get_obj(kungfu::yijinjing::IControlCenter* cc, const std::string& cfg_name);
EXPORT_FLAG const char* sze_strategy_build_id();

#ifdef __cplusplus
}
#endif

const char* sze_strategy_build_id() {
    return "sze-strategy-20260811-turnover-arbiter-v2";
}

IWCStrategy* get_obj(kungfu::yijinjing::IControlCenter* cc, const std::string& cfg_name) {
    std::cerr << "[sze_get_obj] build_id=" << sze_strategy_build_id() << '\n';
    const std::string filename = cfg_name.empty() ? "config_sze.json" : cfg_name;
    json jconfig;
    if (!file2json(filename, jconfig)) {
        return nullptr;
    }

    std::string config_error;
    if (!sze_strategy_library::validate_config(jconfig, &config_error)) {
        std::cerr << "[sze_get_obj] " << config_error << '\n';
        return nullptr;
    }

    std::string strategy_name = "sze_strategy";
    if (jconfig.find("strategy_name") != jconfig.end()) {
        strategy_name = jconfig["strategy_name"].get<std::string>();
    } else if (jconfig.find("name") != jconfig.end()) {
        strategy_name = jconfig["name"].get<std::string>();
    }

    auto* strategy = new StrategyBase(strategy_name, jconfig);
    strategy->set_cc(cc);
    if (cc != nullptr) {
        try {
            (void)cc->get_rid_pair(strategy_name);
        } catch (const std::exception& e) {
            std::cerr << "[sze_get_obj] get_rid_pair threw: " << e.what() << '\n';
        }
        cc->set_str(reinterpret_cast<void*>(strategy));
    }
    strategy->init();
    if (!strategy->sze_recovery_consumer_started()) {
        std::cerr << "[sze_get_obj] recovery consumer failed to start" << '\n';
        delete strategy;
        return nullptr;
    }
    std::cerr << "[sze_get_obj] recovery_consumer_started=1 consumer_attached=1" << '\n';
    strategy->start();
    return strategy;
}
