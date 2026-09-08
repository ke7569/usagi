#include "adapters/deepwin/sse/sse_config_guard.h"
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
        {"mode", "hardware-gap-batch"},
        {"threshold_ns", 5000},
        {"comparison", "greater-or-equal"},
        {"clock", "NIC_PHC"},
        {"candidate_event", "CompleteOrderBookSH Level2"},
        {"activity_scope", "per-udp-subscription-gap"},
        {"same_exchange_time_policy", "at-most-one-sample"},
        {"initial_window", "first-valid-book-at-or-after-open"},
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
    config["sse_live_sampling"]["threshold_ns"] = 4999;
    if (sse_strategy_library::validate_config(config, &error) ||
        error.find("threshold_ns=5000") == std::string::npos) {
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
    const char* sampling_fields[] = {
        "activity_scope", "same_exchange_time_policy", "initial_window"
    };
    const char* rejected_values[] = {
        "per-instrument-sse-book-update", "allow-repeated-samples", "09:25:00"
    };
    for (unsigned i = 0; i < 3; ++i) {
        config = valid_config();
        config["sse_live_sampling"][sampling_fields[i]] = rejected_values[i];
        if (sse_strategy_library::validate_config(config, &error) ||
            error.find(sampling_fields[i]) == std::string::npos) {
            std::cerr << "unsupported SSE sampling policy accepted: " << sampling_fields[i]
                      << ": " << error << std::endl;
            return 13;
        }
        config = valid_config();
        config["sse_live_sampling"].erase(sampling_fields[i]);
        if (sse_strategy_library::validate_config(config, &error) ||
            error.find(sampling_fields[i]) == std::string::npos) {
            std::cerr << "missing SSE sampling policy accepted: " << sampling_fields[i]
                      << ": " << error << std::endl;
            return 14;
        }
    }
    if (argc == 2) {
        std::ifstream input(argv[1]);
        nlohmann::json generated;
        if (!input.is_open()) {
            std::cerr << "cannot open generated SSE config: " << argv[1] << std::endl;
            return 10;
        }
        try {
            input >> generated;
        } catch (...) {
            std::cerr << "cannot parse generated SSE config" << std::endl;
            return 11;
        }
        if (!sse_strategy_library::validate_config(generated, &error)) {
            std::cerr << "generated SSE config rejected: " << error << std::endl;
            return 12;
        }
    }
    std::cout << "sse_config_guard_test: PASS" << std::endl;
    return 0;
}
