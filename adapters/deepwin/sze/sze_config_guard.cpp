#include "adapters/deepwin/sze/sze_config_guard.h"

#include <sched.h>
#include <set>

namespace sze_strategy_library {

namespace {

bool reject(const char* message, std::string* error) {
    if (error != 0) {
        *error = message;
    }
    return false;
}

std::string configured_mode(const nlohmann::json& config) {
    const char* keys[] = {"sz_orderbook_mode", "mode", "orderbook_mode"};
    for (std::size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        nlohmann::json::const_iterator item = config.find(keys[index]);
        if (item != config.end() && item->is_string()) {
            return item->get<std::string>();
        }
    }
    return std::string();
}

bool is_hp_mode(const nlohmann::json& config) {
    const std::string value = configured_mode(config);
    return value == "hp-shadow" || value == "hp_shadow" ||
           value == "hp-realtime" || value == "hp_realtime";
}

bool is_hp_shadow_mode(const nlohmann::json& config) {
    const std::string value = configured_mode(config);
    return value == "hp-shadow" || value == "hp_shadow";
}

bool is_hp_realtime_mode(const nlohmann::json& config) {
    const std::string value = configured_mode(config);
    return value == "hp-realtime" || value == "hp_realtime";
}

nlohmann::json::const_iterator find_capture(const nlohmann::json& config) {
    nlohmann::json::const_iterator capture = config.find("sze_prediction_capture");
    if (capture == config.end()) {
        capture = config.find("mix153060_capture");
    }
    return capture;
}

bool validate_live_routing(const nlohmann::json& config, std::string* error) {
    nlohmann::json::const_iterator routing = config.find("sze_order_routing");
    if (routing == config.end() || !routing->is_object()) {
        return reject("hp-realtime requires explicit sze_order_routing object", error);
    }
    nlohmann::json::const_iterator enabled = routing->find("enabled");
    nlohmann::json::const_iterator mode = routing->find("mode");
    if (enabled == routing->end() || !enabled->is_boolean() || !enabled->get<bool>() ||
        mode == routing->end() || !mode->is_string() ||
        (mode->get<std::string>() != "live" && mode->get<std::string>() != "virtual")) {
        return reject("hp-realtime requires enabled sze_order_routing mode live or virtual", error);
    }
    nlohmann::json::const_iterator retry_ms =
        routing->find("position_query_retry_ms");
    if (retry_ms != routing->end() &&
        (!retry_ms->is_number_integer() || retry_ms->get<int>() < 1000)) {
        return reject("position_query_retry_ms must be an integer >= 1000", error);
    }
    nlohmann::json::const_iterator cutoff =
        routing->find("position_query_cutoff_hhmmss");
    if (cutoff != routing->end()) {
        if (!cutoff->is_number_integer()) {
            return reject("position_query_cutoff_hhmmss must be valid HHMMSS", error);
        }
        const int value = cutoff->get<int>();
        const int hour = value / 10000;
        const int minute = (value / 100) % 100;
        const int second = value % 100;
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
            second < 0 || second > 59) {
            return reject("position_query_cutoff_hhmmss must be valid HHMMSS", error);
        }
    }
    nlohmann::json::const_iterator test_order = config.find("sze_test_order");
    if (test_order != config.end()) {
        if (!test_order->is_object()) {
            return reject("sze_test_order must be an object", error);
        }
        nlohmann::json::const_iterator test_enabled = test_order->find("enabled");
        if (test_enabled != test_order->end() && !test_enabled->is_boolean()) {
            return reject("sze_test_order.enabled must be boolean", error);
        }
        if (test_enabled != test_order->end() && test_enabled->get<bool>() &&
            mode->get<std::string>() != "live") {
            return reject("sze_test_order requires live routing mode", error);
        }
        nlohmann::json::const_iterator volume = test_order->find("volume");
        if (volume != test_order->end() &&
            (!volume->is_number_integer() || volume->get<int>() <= 0)) {
            return reject("sze_test_order.volume must be a positive integer", error);
        }
        nlohmann::json::const_iterator side = test_order->find("side");
        if (side != test_order->end() &&
            (!side->is_string() || (side->get<std::string>() != "buy" &&
                                    side->get<std::string>() != "sell"))) {
            return reject("sze_test_order.side must be buy or sell", error);
        }
        nlohmann::json::const_iterator price = test_order->find("price");
        if (price != test_order->end() &&
            (!price->is_number() || price->get<double>() < 0.0)) {
            return reject("sze_test_order.price must be non-negative", error);
        }
    }
    nlohmann::json::const_iterator capture = find_capture(config);
    if (capture != config.end() && capture->is_object()) {
        nlohmann::json::const_iterator capture_only = capture->find("capture_only");
        if (capture_only != capture->end() && capture_only->is_boolean() &&
            capture_only->get<bool>()) {
            return reject("sze_order_routing rejects capture_only", error);
        }
    }
    nlohmann::json::const_iterator recovery = config.find("sze_recovery_consumer");
    bool recovery_trading = false;
    if (recovery != config.end() && recovery->is_object()) {
        nlohmann::json::const_iterator recovery_enabled = recovery->find("enabled");
        if (recovery_enabled != recovery->end() && recovery_enabled->is_boolean() &&
            recovery_enabled->get<bool>()) {
            nlohmann::json::const_iterator trading_enabled = recovery->find("trading_enabled");
            recovery_trading = trading_enabled != recovery->end() &&
                trading_enabled->is_boolean() && trading_enabled->get<bool>();
            if (!recovery_trading) {
                return reject("recovery trading requires explicit trading_enabled", error);
            }
        }
    }
    nlohmann::json::const_iterator md_sources = config.find("md_source_index");
    nlohmann::json::const_iterator td_sources = config.find("td_source_index");
    if (md_sources == config.end() || !md_sources->is_array() ||
        (!recovery_trading && md_sources->empty()) ||
        td_sources == config.end() || !td_sources->is_array() || td_sources->size() != 1 ||
        !td_sources->at(0).is_number_integer()) {
        return reject("sze_order_routing requires market source or recovery handoff and one td_source_index", error);
    }
    if (recovery_trading) {
        nlohmann::json::const_iterator handoff = routing->find("input_mode");
        if (handoff == routing->end() || !handoff->is_string() ||
            handoff->get<std::string>() != "recovery_handoff") {
            return reject("recovery trading requires input_mode=recovery_handoff", error);
        }
        const char* required_limits[] = {
            "max_order_volume", "max_position"
        };
        for (std::size_t i = 0; i < sizeof(required_limits) / sizeof(required_limits[0]); ++i) {
            nlohmann::json::const_iterator limit = routing->find(required_limits[i]);
            if (limit == routing->end() || !limit->is_number_integer() ||
                limit->get<int>() <= 0) {
                return reject("recovery trading requires positive integer order limits", error);
            }
        }
    }
    return true;
}

bool validate_startup_warmup(const nlohmann::json& config, std::string* error) {
    nlohmann::json::const_iterator warmup =
        config.find("sze_startup_warmup_signals");
    if (warmup == config.end()) {
        return true;
    }
    if (!warmup->is_number_integer() || warmup->get<int>() < 0) {
        return reject(
            "sze_startup_warmup_signals must be a non-negative integer", error);
    }
    return true;
}

bool validate_worker_plan(const nlohmann::json& config, std::string* error) {
    nlohmann::json::const_iterator count = config.find("worker_count");
    nlohmann::json::const_iterator cpus = config.find("worker_cpus");
    nlohmann::json::const_iterator state_cpus = config.find("worker_state_cpus");
    if (count == config.end() && cpus == config.end()) {
        return true;
    }
    if (count == config.end() || !count->is_number_integer() ||
        count->get<int>() < 1 || count->get<int>() > 64) {
        return reject("worker_count must be an integer in [1,64]", error);
    }
    if (cpus == config.end() || !cpus->is_array() ||
        static_cast<int>(cpus->size()) != count->get<int>()) {
        return reject("worker_cpus must contain exactly worker_count entries", error);
    }
    if (state_cpus == config.end() || !state_cpus->is_array() ||
        static_cast<int>(state_cpus->size()) != count->get<int>()) {
        return reject("worker_state_cpus must contain exactly worker_count entries", error);
    }
    std::set<int> seen;
    for (nlohmann::json::const_iterator it = cpus->begin(); it != cpus->end(); ++it) {
        if (!it->is_number_integer() || it->get<int>() < 0 ||
            it->get<int>() >= CPU_SETSIZE || !seen.insert(it->get<int>()).second) {
            return reject("worker_cpus must contain unique valid CPU ids", error);
        }
    }
    for (nlohmann::json::const_iterator it = state_cpus->begin();
         it != state_cpus->end(); ++it) {
        if (!it->is_number_integer() || it->get<int>() < 0 ||
            it->get<int>() >= CPU_SETSIZE || !seen.insert(it->get<int>()).second) {
            return reject("worker strategy/state CPUs must be unique valid CPU ids", error);
        }
    }
    nlohmann::json::const_iterator params = config.find("ins_params");
    if (params != config.end() && params->is_object()) {
        for (nlohmann::json::const_iterator it = params->begin();
             it != params->end(); ++it) {
            if (!it->is_object()) {
                return reject("ins_params entries must be objects", error);
            }
            nlohmann::json::const_iterator cpu = it->find("cpu");
            if (cpu != it->end() && (!cpu->is_number_integer() ||
                cpu->get<int>() < 0 || cpu->get<int>() >= CPU_SETSIZE)) {
                return reject("ins_params.cpu must be a valid CPU id", error);
            }
        }
    }
    return true;
}

}  // namespace

bool validate_config(const nlohmann::json& config, std::string* error) {
    if (error != 0) {
        error->clear();
    }
    if (!validate_startup_warmup(config, error)) {
        return false;
    }
    if (!validate_worker_plan(config, error)) {
        return false;
    }
    nlohmann::json::const_iterator market = config.find("market");
    if (market == config.end()) {
        return reject("SZE strategy library requires config field market=SZ", error);
    }
    if (!market->is_string()) {
        return reject("SZE strategy library requires string config field market=SZ", error);
    }
    const std::string value = market->get<std::string>();
    if (value == "SH") {
        return reject(
            "SZE strategy library rejects market=SH; Shanghai requires a separate strategy library",
            error);
    }
    if (value != "SZ") {
        return reject(
            "SZE strategy library rejects non-SZ market; a separate strategy library is required",
            error);
    }
    if (!is_hp_mode(config)) {
        return true;
    }
    if (config.find("mix153060_model_artifact") != config.end() ||
        config.find("hp_model_artifact") != config.end()) {
        return reject(
            "HP-mode Shenzhen config accepts only model_path; remove model artifact aliases",
            error);
    }
    nlohmann::json::const_iterator model_path = config.find("model_path");
    if (model_path == config.end() || !model_path->is_string() ||
        model_path->get<std::string>().empty()) {
        return reject("HP-mode Shenzhen config requires non-empty string model_path", error);
    }
    nlohmann::json::const_iterator routing = config.find("sze_order_routing");
    if (routing != config.end() && routing->is_object()) {
        nlohmann::json::const_iterator enabled = routing->find("enabled");
        if (enabled != routing->end() && enabled->is_boolean() && enabled->get<bool>() &&
            !is_hp_realtime_mode(config)) {
            return reject("sze_order_routing requires hp-realtime mode", error);
        }
    }
    if (is_hp_realtime_mode(config) && !validate_live_routing(config, error)) {
        return false;
    }
    nlohmann::json::const_iterator recovery =
        config.find("sze_recovery_consumer");
    if (recovery == config.end()) {
        return true;
    }
    if (!recovery->is_object()) {
        return reject("sze_recovery_consumer must be an object", error);
    }
    nlohmann::json::const_iterator enabled = recovery->find("enabled");
    if (enabled == recovery->end() || !enabled->is_boolean() ||
        !enabled->get<bool>()) {
        if (recovery->find("allow_invalid_replay_for_analysis") != recovery->end() &&
            recovery->at("allow_invalid_replay_for_analysis").is_boolean() &&
            recovery->at("allow_invalid_replay_for_analysis").get<bool>()) {
            return reject(
                "allow_invalid_replay_for_analysis requires enabled recovery consumer",
                error);
        }
        return true;
    }
    nlohmann::json::const_iterator analysis =
        recovery->find("allow_invalid_replay_for_analysis");
    if (analysis != recovery->end() && !analysis->is_boolean()) {
        return reject(
            "allow_invalid_replay_for_analysis must be boolean", error);
    }
    if (analysis != recovery->end() && analysis->is_boolean() &&
        analysis->get<bool>()) {
        if (!is_hp_shadow_mode(config)) {
            return reject(
                "allow_invalid_replay_for_analysis requires hp-shadow mode", error);
        }
        nlohmann::json::const_iterator capture = find_capture(config);
        if (capture == config.end() || !capture->is_object() ||
            capture->find("enabled") == capture->end() ||
            !capture->at("enabled").is_boolean() ||
            !capture->at("enabled").get<bool>() ||
            capture->find("capture_only") == capture->end() ||
            !capture->at("capture_only").is_boolean() ||
            !capture->at("capture_only").get<bool>() ||
            capture->find("samples") == capture->end() ||
            !capture->at("samples").is_boolean() ||
            !capture->at("samples").get<bool>()) {
            return reject(
                "allow_invalid_replay_for_analysis requires enabled capture_only samples",
                error);
        }
        nlohmann::json::const_iterator td_sources = config.find("td_source_index");
        if (td_sources == config.end() || !td_sources->is_array() ||
            !td_sources->empty()) {
            return reject(
                "allow_invalid_replay_for_analysis requires empty td_source_index",
                error);
        }
        nlohmann::json::const_iterator vtd_sources = config.find("vtd");
        if (vtd_sources == config.end() || !vtd_sources->is_array() ||
            !vtd_sources->empty()) {
            return reject(
                "allow_invalid_replay_for_analysis requires explicit empty vtd",
                error);
        }
    }
    nlohmann::json::const_iterator md_sources = config.find("md_source_index");
    if (md_sources != config.end() &&
        (!md_sources->is_array() || !md_sources->empty())) {
        return reject(
            "sze_recovery_consumer requires empty md_source_index to prevent duplicate delivery",
            error);
    }
    const bool analysis_mode = analysis != recovery->end() &&
        analysis->is_boolean() && analysis->get<bool>();
    const char* required_strings[] = {
        "journal_directory", "journal_prefix"
    };
    for (std::size_t index = 0;
         index < sizeof(required_strings) / sizeof(required_strings[0]); ++index) {
        nlohmann::json::const_iterator item =
            recovery->find(required_strings[index]);
        if (item == recovery->end() || !item->is_string() ||
            item->get<std::string>().empty()) {
            return reject(
                "sze_recovery_consumer requires journal_directory and journal_prefix",
                error);
        }
    }
    if (!analysis_mode) {
        nlohmann::json::const_iterator shm_path = recovery->find("shm_path");
        if (shm_path == recovery->end() || !shm_path->is_string() ||
            shm_path->get<std::string>().empty()) {
            return reject(
                "sze_recovery_consumer requires shm_path for online handoff", error);
        }
    }
    nlohmann::json::const_iterator trading_day = recovery->find("trading_day");
    nlohmann::json::const_iterator state_cpu = recovery->find("state_cpu");
    nlohmann::json::const_iterator strategy_cpu = recovery->find("strategy_cpu");
    if (trading_day == recovery->end() || !trading_day->is_number() ||
        state_cpu == recovery->end() || !state_cpu->is_number() ||
        strategy_cpu == recovery->end() || !strategy_cpu->is_number()) {
        return reject(
            "sze_recovery_consumer requires numeric trading_day, state_cpu, and strategy_cpu",
            error);
    }
    if (state_cpu->get<int>() == strategy_cpu->get<int>()) {
        return reject(
            "sze_recovery_consumer state_cpu and strategy_cpu must be distinct",
            error);
    }
    return true;
}

}  // namespace sze_strategy_library
