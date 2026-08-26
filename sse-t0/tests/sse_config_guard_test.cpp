#include "../strategy/sse_config_guard.h"
#include <fstream>
#include <iostream>

namespace {
nlohmann::json valid_config() {
    nlohmann::json config;
    config["market"] = "SH";
    config["model_type"] = "sse_hybrid_native";
    config["model_path"] = "/opt/sse-t0/models/hybrid/models/tick/ssemodl1.bin";
    config["snapshot_baseline_model_path"] = "/opt/sse-t0/models/hybrid/models/snapshot/baseline.ssegru";
    config["snapshot_baseline_scaler_path"] = "/opt/sse-t0/models/hybrid/models/snapshot/baseline.json";
    config["snapshot_auction59_model_path"] = "/opt/sse-t0/models/hybrid/models/snapshot/auction59.ssegru";
    config["snapshot_auction59_scaler_path"] = "/opt/sse-t0/models/hybrid/models/snapshot/auction59.json";
    config["sse_hybrid_routing_path"] = "/opt/sse-t0/models/hybrid/config/sse_hybrid_routing.json";
    config["sse_factor_contract"] = "v0.4-sse-cob-batch-end-100us";
    config["sse_inference_backend"] = "native-cpp";
    config["runtime_mode"] = "prediction-only";
    config["prediction_only"] = true;
    config["trading_enabled"] = false;
    config["production_approval"] = false;
    config["td_source_index"] = nlohmann::json::array();
    config["sse_live_sampling"] = {
        {"mode", "trailing-edge-one-shot"},
        {"threshold_ns", 100000},
        {"comparison", "strict-greater-than"},
        {"clock", "CLOCK_MONOTONIC"},
        {"candidate_event", "CompleteOrderBookSH Level2"},
        {"activity_scope", "normalized-sse-book-update"},
        {"sequence_gap_policy", "fail-closed"},
        {"periodic_md", false},
        {"shutdown_flush", false}
    };
    config["model_routing"] = {
        {"clock", "exchange-time-of-day-micros"},
        {"snapshot_selected_window", "[09:30:00,09:35:00)"},
        {"tick_selected_window", "[09:35:00,24:00:00)"},
        {"tick_warm_before_switch", true},
        {"silent_fallback", false}
    };
    config["feed_health"] = {
        {"require_sequence_healthy", true},
        {"sample_on_gap", false},
        {"resume_after_explicit_resync", true}
    };
    return config;
}
}

int main(int argc, char** argv) {
    nlohmann::json config = valid_config();
    std::string error = "stale";
    if (!sse_strategy_library::validate_config(config, &error) || !error.empty()) {
        std::cerr << "valid SSE config rejected: " << error << std::endl;
        return 1;
    }
    config["market"] = "SZ";
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("non-SH") == std::string::npos) {
        std::cerr << "non-SH config accepted: " << error << std::endl;
        return 2;
    }
    config["market"] = "SH";
    config.erase("model_path");
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("model_path") == std::string::npos) {
        std::cerr << "missing model config accepted: " << error << std::endl;
        return 3;
    }
    config["model_path"] = "/opt/sse-t0/models/hybrid/models/tick/ssemodl1.bin";
    config["sse_factor_contract"] = "mix153060";
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("factor contract") == std::string::npos) {
        std::cerr << "SZE factor contract accepted by SSE config" << std::endl;
        return 4;
    }
    config["sse_factor_contract"] = "v0.4-sse-cob-batch-end-100us";
    config["sse_inference_backend"] = "torch";
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("native-cpp") == std::string::npos) {
        std::cerr << "Torch backend accepted by SSE config" << std::endl;
        return 5;
    }
    config = valid_config();
    config["sse_live_sampling"]["threshold_ns"] = 99999;
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("threshold_ns=100000") == std::string::npos) {
        std::cerr << "wrong SSE batch threshold accepted: " << error << std::endl;
        return 6;
    }
    config = valid_config();
    config["sse_live_sampling"]["periodic_md"] = true;
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("periodic_md=false") == std::string::npos) {
        std::cerr << "periodic SSE MD accepted: " << error << std::endl;
        return 7;
    }
    config = valid_config();
    config["trading_enabled"] = true;
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("trading_enabled=false") == std::string::npos) {
        std::cerr << "SSE trading enabled in prediction-only config: " << error << std::endl;
        return 8;
    }
    config = valid_config();
    config["td_source_index"].push_back(190);
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("empty td_source_index") == std::string::npos) {
        std::cerr << "SSE TD source accepted in prediction-only config: " << error << std::endl;
        return 9;
    }
    config = valid_config();
    config["runtime_mode"] = "live";
    config["prediction_only"] = false;
    config["trading_enabled"] = true;
    config["td_source_index"].push_back(190);
    config["sse_order_routing"] = {
        {"enabled", true}, {"mode", "live"}, {"td_source", 190}
    };
    if (!sse_strategy_library::validate_config(config, &error)) {
        std::cerr << "staged SSE live config rejected: " << error << std::endl;
        return 10;
    }
    config["sse_test_order"] = {{"enabled", true}};
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("production_approval") == std::string::npos) {
        std::cerr << "unapproved SSE test order accepted: " << error << std::endl;
        return 11;
    }
    config["production_approval"] = true;
    if (!sse_strategy_library::validate_config(config, &error)) {
        std::cerr << "approved SSE live config rejected: " << error << std::endl;
        return 12;
    }
    if (argc == 2) {
        std::ifstream input(argv[1]);
        nlohmann::json generated;
        if (!input.is_open()) {
            std::cerr << "cannot open generated SSE config: " << argv[1] << std::endl;
            return 13;
        }
        try {
            input >> generated;
        } catch (...) {
            std::cerr << "cannot parse generated SSE config" << std::endl;
            return 14;
        }
        if (!sse_strategy_library::validate_config(generated, &error)) {
            std::cerr << "generated SSE config rejected: " << error << std::endl;
            return 15;
        }
    }
    std::cout << "sse_config_guard_test: PASS" << std::endl;
    return 0;
}
