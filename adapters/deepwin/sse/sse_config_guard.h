#ifndef T0_MAIN_DEEPWIN_STRATEGY_SSE_CONFIG_GUARD_H
#define T0_MAIN_DEEPWIN_STRATEGY_SSE_CONFIG_GUARD_H
#include "third_party/nlohmann/json.hpp"
#include <string>
namespace sse_strategy_library {
bool validate_config(const nlohmann::json& config, std::string* error);
}
#endif
