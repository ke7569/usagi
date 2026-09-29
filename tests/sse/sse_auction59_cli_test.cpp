#include "apps/StreamProcessingCli.h"
#include "tests/sse/sse_test_artifacts.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>

int main() {
    char pattern[] = "/tmp/sse-auction-cli-XXXXXX";
    const char* created = ::mkdtemp(pattern);
    assert(created);
    const std::string dir(created);
    sse_test_artifacts::write_tick_artifact(dir + "/tick.bin");
    sse_test_artifacts::write_snapshot_artifact(dir + "/baseline.bin", 36);
    sse_test_artifacts::write_snapshot_artifact(dir + "/auction.bin", 95);
    sse_test_artifacts::write_scaler(dir + "/baseline.json", 36);
    sse_test_artifacts::write_scaler(dir + "/auction.json", 95);
    typedef nlohmann::json Json;
    Json profile = {
        {"schema_version", 1}, {"market", "SH"}, {"execution", "disabled"},
        {"processing_mode", "prediction"}, {"processing_contract", "sse-per-instrument-v2"},
        {"trading_day", 20260909}, {"processing_sha256", std::string(64, '0')},
        {"environment", {{"execution", "disabled"}, {"mode", "live"}, {"clock", "host"}}},
        {"prediction", {
            {"model_path", dir + "/tick.bin"},
            {"snapshot_baseline_model_path", dir + "/baseline.bin"},
            {"snapshot_baseline_scaler_path", dir + "/baseline.json"},
            {"snapshot_auction59_model_path", dir + "/auction.bin"},
            {"snapshot_auction59_scaler_path", dir + "/auction.json"}}},
        {"instruments", Json::array({{
            {"instrument", "600000"}, {"trading_date", 20260909},
            {"average_amount", 8000000}, {"turnover_threshold", 1000},
            {"free_share", 10000000}, {"pre_close", 10},
            {"upper_limit", 11}, {"lower_limit", 9}, {"history_volatility_20d", 0.02}}})}
    };
    // Both stream and journal hosts construct this application before the
    // opening auction. No factor file exists, even for old profiles naming one.
    for (int mode = 0; mode < 3; ++mode) {
        if (mode == 1) profile["prediction"]["auction59"] = {{"enabled", true}};
        if (mode == 2) {
            profile["prediction"]["auction59"] = {{"enabled", false}};
            profile["prediction"]["snapshot_auction59_factors_path"] = dir + "/absent.csv";
        }
        {
            std::ofstream out((dir + "/profile.json").c_str());
            out << profile.dump();
        }
        sse_application::StreamProcessingCli app(dir + "/profile.json", true, dir, 2,
                                                []() { return true; });
        assert(app.processing_valid());
        assert(app.summary().at("predictions").get<int>() == 0);
    }
    const char* files[] = {"tick.bin", "baseline.bin", "auction.bin", "baseline.json",
                           "auction.json", "profile.json"};
    for (const char* file : files) assert(::unlink((dir + "/" + file).c_str()) == 0);
    assert(::rmdir(dir.c_str()) == 0);
    std::cout << "sse_auction59_cli_test ok\n";
}
