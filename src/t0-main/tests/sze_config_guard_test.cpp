#include "../deepwin_strategy/sze_config_guard.h"

#include <iostream>
#include <string>

namespace {

bool expect_rejected(const nlohmann::json& config, const std::string& expected_text) {
    std::string error;
    if (sze_strategy_library::validate_config(config, &error)) {
        std::cerr << "expected rejected config" << std::endl;
        return false;
    }
    if (error.find(expected_text) == std::string::npos) {
        std::cerr << "unexpected rejection: " << error << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    nlohmann::json sze;
    sze["market"] = "SZ";
    std::string error = "stale";
    if (!sze_strategy_library::validate_config(sze, &error) || !error.empty()) {
        std::cerr << "SZ config was rejected: " << error << std::endl;
        return 1;
    }

    nlohmann::json sse;
    sse["market"] = "SH";
    if (!expect_rejected(sse, "Shanghai requires a separate strategy library")) {
        return 2;
    }

    nlohmann::json missing;
    if (!expect_rejected(missing, "requires config field market=SZ")) {
        return 3;
    }

    nlohmann::json malformed;
    malformed["market"] = 89;
    if (!expect_rejected(malformed, "requires string config field market=SZ")) {
        return 4;
    }

    nlohmann::json other;
    other["market"] = "BJ";
    if (!expect_rejected(other, "rejects non-SZ market")) {
        return 5;
    }

    nlohmann::json hp;
    hp["market"] = "SZ";
    hp["sz_orderbook_mode"] = "hp-shadow";
    hp["model_path"] = "/tmp/mix153060.bin";
    error = "stale";
    if (!sze_strategy_library::validate_config(hp, &error) || !error.empty()) {
        std::cerr << "HP config was rejected: " << error << std::endl;
        return 6;
    }

    nlohmann::json compact_hp = hp;
    compact_hp.erase("sz_orderbook_mode");
    compact_hp["mode"] = "hp-shadow";
    error = "stale";
    if (!sze_strategy_library::validate_config(compact_hp, &error) || !error.empty()) {
        std::cerr << "compact HP config was rejected: " << error << std::endl;
        return 27;
    }

    nlohmann::json hp_missing = hp;
    hp_missing.erase("model_path");
    if (!expect_rejected(hp_missing, "requires non-empty string model_path")) {
        return 7;
    }

    nlohmann::json hp_alias = hp;
    hp_alias["mix153060_model_artifact"] = "/tmp/other.bin";
    if (!expect_rejected(hp_alias, "accepts only model_path")) {
        return 8;
    }

    nlohmann::json realtime = hp;
    realtime["sz_orderbook_mode"] = "hp-realtime";
    if (!expect_rejected(realtime, "requires explicit sze_order_routing object")) {
        return 9;
    }
    realtime["md_source_index"] = {88};
    realtime["td_source_index"] = {180};
    realtime["sze_order_routing"] = {{"enabled", true}, {"mode", "virtual"}};
    error = "stale";
    if (!sze_strategy_library::validate_config(realtime, &error) || !error.empty()) {
        std::cerr << "virtual live config was rejected: " << error << std::endl;
        return 10;
    }
    nlohmann::json test_order = realtime;
    test_order["sze_order_routing"]["mode"] = "live";
    test_order["sze_test_order"] = {
        {"enabled", true}, {"instrument", "000001.SZ"}, {"side", "buy"},
        {"price", 0.0}, {"volume", 100}, {"trigger_after_signals", 1},
        {"cancel_delay_ms", 1000}
    };
    error = "stale";
    if (!sze_strategy_library::validate_config(test_order, &error) || !error.empty()) {
        std::cerr << "test order config was rejected: " << error << std::endl;
        return 11;
    }
    nlohmann::json virtual_test_order = realtime;
    virtual_test_order["sze_test_order"] = {{"enabled", true}};
    if (!expect_rejected(virtual_test_order, "requires live routing mode")) {
        return 12;
    }
    nlohmann::json bad_test_order = test_order;
    bad_test_order["sze_test_order"]["volume"] = 0;
    if (!expect_rejected(bad_test_order, "positive integer")) {
        return 13;
    }
    nlohmann::json realtime_capture = realtime;
    realtime_capture["mix153060_capture"] = {{"capture_only", true}};
    if (!expect_rejected(realtime_capture, "rejects capture_only")) {
        return 14;
    }
    nlohmann::json realtime_multi_td = realtime;
    realtime_multi_td["td_source_index"] = {180, 181};
    if (!expect_rejected(realtime_multi_td, "one td_source_index")) {
        return 15;
    }
    nlohmann::json realtime_recovery = realtime;
    realtime_recovery["sze_recovery_consumer"] = {{"enabled", true}};
    if (!expect_rejected(realtime_recovery, "requires explicit trading_enabled")) {
        return 16;
    }
    realtime_recovery["md_source_index"] = nlohmann::json::array();
    realtime_recovery["sze_recovery_consumer"] = {
        {"enabled", true},
        {"trading_enabled", true},
        {"trading_day", 20260810},
        {"journal_directory", "/home/zane/data/sze_journal_20260810"},
        {"journal_prefix", "sze_all"},
        {"shm_path", "/dev/shm/sze_all_20260810.events"},
        {"state_cpu", 34},
        {"strategy_cpu", 35}
    };
    realtime_recovery["sze_order_routing"] = {
        {"enabled", true},
        {"mode", "live"},
        {"input_mode", "recovery_handoff"},
        {"max_position", 200},
    };
    error = "stale";
    if (!sze_strategy_library::validate_config(realtime_recovery, &error) ||
        !error.empty()) {
        std::cerr << "recovery trading config was rejected: " << error << std::endl;
        return 28;
    }
    nlohmann::json recovery_without_buy_limit = realtime_recovery;
    recovery_without_buy_limit["sze_order_routing"].erase("max_position");
    if (!expect_rejected(recovery_without_buy_limit, "positive integer max_position")) {
        return 281;
    }
    nlohmann::json invalid_position_retry = realtime_recovery;
    invalid_position_retry["sze_order_routing"]["position_query_retry_ms"] = 999;
    if (!expect_rejected(invalid_position_retry, "integer >= 1000")) {
        return 29;
    }
    nlohmann::json invalid_position_cutoff = realtime_recovery;
    invalid_position_cutoff["sze_order_routing"]["position_query_cutoff_hhmmss"] = 93699;
    if (!expect_rejected(invalid_position_cutoff, "valid HHMMSS")) {
        return 30;
    }
    nlohmann::json warmup = realtime;
    warmup["sze_startup_warmup_signals"] = 50;
    error = "stale";
    if (!sze_strategy_library::validate_config(warmup, &error) || !error.empty()) {
        std::cerr << "warmup config was rejected: " << error << std::endl;
        return 31;
    }
    nlohmann::json bad_warmup = realtime;
    bad_warmup["sze_startup_warmup_signals"] = -1;
    if (!expect_rejected(bad_warmup, "non-negative integer")) {
        return 32;
    }
    nlohmann::json non_integer_warmup = realtime;
    non_integer_warmup["sze_startup_warmup_signals"] = 50.5;
    if (!expect_rejected(non_integer_warmup, "non-negative integer")) {
        return 33;
    }
    nlohmann::json worker_plan = realtime;
    worker_plan["worker_count"] = 2;
    worker_plan["worker_cpus"] = {16, 17};
    worker_plan["worker_state_cpus"] = {24, 25};
    worker_plan["ins_params"] = {
        {"000001.SZ", {{"Date", 20260810}, {"cpu", 16}}},
        {"000002.SZ", {{"Date", 20260810}, {"cpu", 17}}}
    };
    error = "stale";
    if (!sze_strategy_library::validate_config(worker_plan, &error) || !error.empty()) {
        std::cerr << "worker plan config was rejected: " << error << std::endl;
        return 34;
    }
    nlohmann::json duplicate_worker_cpu = worker_plan;
    duplicate_worker_cpu["worker_cpus"] = {16, 16};
    if (!expect_rejected(duplicate_worker_cpu, "unique valid CPU")) {
        return 35;
    }
    nlohmann::json shadow_routing = hp;
    shadow_routing["sze_order_routing"] = {{"enabled", true}, {"mode", "virtual"}};
    if (!expect_rejected(shadow_routing, "requires hp-realtime mode")) {
        return 17;
    }

    nlohmann::json recovery = hp;
    recovery["md_source_index"] = nlohmann::json::array();
    recovery["sze_recovery_consumer"] = {
        {"enabled", true},
        {"trading_day", 20260722},
        {"journal_directory", "/home/zane/data/sze_journal"},
        {"journal_prefix", "000001"},
        {"shm_path", "/dev/shm/sze_000001_20260722.events"},
        {"state_cpu", 7},
        {"strategy_cpu", 8}
    };
    error = "stale";
    if (!sze_strategy_library::validate_config(recovery, &error) || !error.empty()) {
        std::cerr << "recovery config was rejected: " << error << std::endl;
        return 18;
    }

    nlohmann::json duplicate_delivery = recovery;
    duplicate_delivery["md_source_index"] = {88};
    if (!expect_rejected(duplicate_delivery, "empty md_source_index")) {
        return 19;
    }

    nlohmann::json missing_shm = recovery;
    missing_shm["sze_recovery_consumer"].erase("shm_path");
    if (!expect_rejected(missing_shm, "requires shm_path for online handoff")) {
        return 20;
    }

    nlohmann::json same_cpu = recovery;
    same_cpu["sze_recovery_consumer"]["strategy_cpu"] = 7;
    if (!expect_rejected(same_cpu, "must be distinct")) {
        return 21;
    }

    nlohmann::json analysis = recovery;
    analysis["td_source_index"] = nlohmann::json::array();
    analysis["vtd"] = nlohmann::json::array();
    analysis["mix153060_capture"] = {
        {"enabled", true},
        {"capture_only", true},
        {"samples", true}
    };
    analysis["sze_recovery_consumer"]["allow_invalid_replay_for_analysis"] = true;
    error = "stale";
    if (!sze_strategy_library::validate_config(analysis, &error) || !error.empty()) {
        std::cerr << "analysis config was rejected: " << error << std::endl;
        return 22;
    }
    analysis["sze_recovery_consumer"].erase("shm_path");
    error = "stale";
    if (!sze_strategy_library::validate_config(analysis, &error) || !error.empty()) {
        std::cerr << "journal-only analysis config was rejected: " << error << std::endl;
        return 23;
    }

    nlohmann::json analysis_live = analysis;
    analysis_live["mix153060_capture"]["capture_only"] = false;
    if (!expect_rejected(analysis_live, "requires enabled capture_only")) {
        return 24;
    }

    nlohmann::json analysis_td = analysis;
    analysis_td["td_source_index"] = {180};
    if (!expect_rejected(analysis_td, "requires empty td_source_index")) {
        return 25;
    }

    nlohmann::json analysis_vtd = analysis;
    analysis_vtd["vtd"] = {{"source", 180}};
    if (!expect_rejected(analysis_vtd, "requires explicit empty vtd")) {
        return 26;
    }

    std::cout << "sze_config_guard_test: PASS" << std::endl;
    return 0;
}
