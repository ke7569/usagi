#include "adapters/deepwin/strategy/StrategyBase.h"
#include "third_party/nlohmann/json.hpp"
#include "adapters/deepwin/sse/sse_config_guard.h"
#include "IControlCenter.h"
#include <fstream>
#include <iostream>
using nlohmann::json;
namespace { bool file2json(const std::string& path, json& out) { std::ifstream file(path.c_str()); if (!file.is_open()) return false; try { file >> out; } catch (...) { return false; } return true; } }
#define EXPORT_FLAG __attribute__((__visibility__("default")))
extern "C" {
EXPORT_FLAG IWCStrategy* get_obj(kungfu::yijinjing::IControlCenter*, const std::string&);
EXPORT_FLAG const char* sse_strategy_build_id();
}
const char* sse_strategy_build_id() { return "sse-strategy-v04-legacy-midmix-sse-20260812"; }
IWCStrategy* get_obj(kungfu::yijinjing::IControlCenter* cc, const std::string& cfg_name) {
    json config; const std::string path = cfg_name.empty() ? "config_sse.json" : cfg_name;
    if (!file2json(path, config)) return nullptr;
    std::string error; if (!sse_strategy_library::validate_config(config, &error)) { std::cerr << "[sse_get_obj] " << error << '\n'; return nullptr; }
    // The installed legacy host has no stop/join hook for our raw-stream
    // workers. The native decoder, factors and strategy are wired through
    // the explicitly owned runtime API; never start an unowned worker here.
    (void)cc;
    std::cerr << "[sse_get_obj] legacy host lifecycle unsupported; use t0_sse_stream or libt0_sse_runtime.so:t0_market_runtime_v1" << '\n';
    return nullptr;
}
