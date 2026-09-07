#include "adapters/deepwin/sse/sse_config_guard.h"

namespace sse_strategy_library {
namespace {

bool reject(const std::string& text, std::string* error) {
    if (error) *error = text;
    return false;
}

bool require_string(const nlohmann::json& object, const char* key,
                    const char* expected, std::string* error) {
    nlohmann::json::const_iterator item = object.find(key);
    if (item == object.end() || !item->is_string() ||
        (expected != 0 && item->get<std::string>() != expected)) {
        return reject(std::string("SSE config requires ") + key +
                      (expected == 0 ? " as a non-empty string" :
                       std::string("=") + expected), error);
    }
    if (expected == 0 && item->get<std::string>().empty()) {
        return reject(std::string("SSE config requires non-empty ") + key, error);
    }
    return true;
}

bool require_false(const nlohmann::json& object, const char* key,
                   std::string* error) {
    nlohmann::json::const_iterator item = object.find(key);
    if (item == object.end() || !item->is_boolean() || item->get<bool>()) {
        return reject(std::string("SSE config requires ") + key + "=false", error);
    }
    return true;
}

bool require_true(const nlohmann::json& object, const char* key,
                  std::string* error) {
    nlohmann::json::const_iterator item = object.find(key);
    if (item == object.end() || !item->is_boolean() || !item->get<bool>()) {
        return reject(std::string("SSE config requires ") + key + "=true", error);
    }
    return true;
}

}  // namespace

bool validate_config(const nlohmann::json& config, std::string* error) {
    if (error) error->clear();
    nlohmann::json::const_iterator market = config.find("market");
    if (market == config.end()) return reject("SSE strategy library requires config field market=SH", error);
    if (!market->is_string()) return reject("SSE strategy library requires string config field market=SH", error);
    if (market->get<std::string>() != "SH") return reject("SSE strategy library rejects non-SH market", error);
    if (!require_string(config, "model_path", 0, error) ||
        !require_string(config, "snapshot_baseline_model_path", 0, error) ||
        !require_string(config, "snapshot_baseline_scaler_path", 0, error) ||
        !require_string(config, "snapshot_auction59_model_path", 0, error) ||
        !require_string(config, "snapshot_auction59_scaler_path", 0, error) ||
        !require_string(config, "sse_hybrid_routing_path", 0, error) ||
        !require_string(config, "model_type", "sse_hybrid_native", error)) return false;
    nlohmann::json::const_iterator factors = config.find("sse_factor_contract");
    if (factors == config.end() || !factors->is_string() ||
        factors->get<std::string>() != "v0.4-sse-cob-batch-end-100us")
        return reject("SSE strategy library requires SSE factor contract v0.4-sse-cob-batch-end-100us", error);
    nlohmann::json::const_iterator backend = config.find("sse_inference_backend");
    if (backend == config.end() || !backend->is_string() ||
        backend->get<std::string>() != "native-cpp")
        return reject("SSE strategy library requires sse_inference_backend=native-cpp", error);

    nlohmann::json::const_iterator sampling = config.find("sse_live_sampling");
    if (sampling == config.end() || !sampling->is_object())
        return reject("SSE config requires sse_live_sampling object", error);
    if (!require_string(*sampling, "mode", "trailing-edge-one-shot", error) ||
        !require_string(*sampling, "comparison", "strict-greater-than", error) ||
        !require_string(*sampling, "clock", "CLOCK_MONOTONIC", error) ||
        !require_string(*sampling, "candidate_event", "CompleteOrderBookSH Level2", error) ||
        !require_string(*sampling, "activity_scope", "per-instrument-sse-book-update", error) ||
        !require_string(*sampling, "same_exchange_time_policy", "at-most-one-sample", error) ||
        !require_string(*sampling, "initial_window", "first-valid-book-at-or-after-open", error) ||
        !require_string(*sampling, "sequence_gap_policy", "fail-closed", error) ||
        !require_false(*sampling, "periodic_md", error) ||
        !require_false(*sampling, "shutdown_flush", error)) return false;
    nlohmann::json::const_iterator threshold = sampling->find("threshold_ns");
    if (threshold == sampling->end() || !threshold->is_number_integer() ||
        threshold->get<long long>() != 100000LL)
        return reject("SSE config requires sse_live_sampling.threshold_ns=100000", error);

    nlohmann::json::const_iterator prediction_only = config.find("prediction_only");
    if (prediction_only == config.end() || !prediction_only->is_boolean() ||
        !prediction_only->get<bool>())
        return reject("SSE live candidate requires prediction_only=true", error);
    if (!require_string(config, "runtime_mode", "prediction-only", error) ||
        !require_false(config, "trading_enabled", error) ||
        !require_false(config, "production_approval", error)) return false;
    nlohmann::json::const_iterator td_sources = config.find("td_source_index");
    if (td_sources == config.end() || !td_sources->is_array() || !td_sources->empty())
        return reject("SSE prediction-only config requires empty td_source_index", error);

    nlohmann::json::const_iterator routing = config.find("model_routing");
    if (routing == config.end() || !routing->is_object())
        return reject("SSE config requires model_routing object", error);
    if (!require_string(*routing, "clock", "exchange-time-of-day-micros", error) ||
        !require_string(*routing, "snapshot_selected_window", "[09:30:00,09:35:00)", error) ||
        !require_string(*routing, "tick_selected_window", "[09:35:00,24:00:00)", error) ||
        !require_true(*routing, "tick_warm_before_switch", error) ||
        !require_false(*routing, "silent_fallback", error)) return false;

    nlohmann::json::const_iterator health = config.find("feed_health");
    if (health == config.end() || !health->is_object())
        return reject("SSE config requires feed_health object", error);
    if (!require_true(*health, "require_sequence_healthy", error) ||
        !require_false(*health, "sample_on_gap", error) ||
        !require_true(*health, "resume_after_explicit_resync", error)) return false;
    return true;
}
}
