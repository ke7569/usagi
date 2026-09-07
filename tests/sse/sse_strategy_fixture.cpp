#include "tests/sse/sse_test_artifacts.h"

#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sse_strategy_fixture NEW_TEST_ASSET_DIRECTORY\n";
        return 2;
    }
    const std::string directory(argv[1]);
    struct stat info;
    if (::stat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        std::cerr << "fixture directory must already exist\n";
        return 2;
    }
    const char* files[] = {"tick.bin", "baseline.ssegru", "auction59.ssegru",
                          "baseline.json", "auction59.json"};
    for (const char* file : files) {
        if (::lstat((directory + "/" + file).c_str(), &info) == 0) {
            std::cerr << "fixture refuses to overwrite existing asset: " << file << '\n';
            return 2;
        }
    }
    // Synthetic test weights: tick final bias is 1.5, both Snapshot arms are zero.
    sse_test_artifacts::write_tick_artifact(directory + "/tick.bin");
    sse_test_artifacts::write_snapshot_artifact(directory + "/baseline.ssegru", 36);
    sse_test_artifacts::write_snapshot_artifact(directory + "/auction59.ssegru", 95);
    sse_test_artifacts::write_scaler(directory + "/baseline.json", 36);
    sse_test_artifacts::write_scaler(directory + "/auction59.json", 95);
    for (const char* file : files) {
        std::ifstream input((directory + "/" + file).c_str(), std::ios::binary | std::ios::ate);
        if (!input || input.tellg() <= 0) {
            std::cerr << "fixture asset write failed: " << file << '\n';
            return 1;
        }
    }
    std::cout << "synthetic_strategy_fixture: tick_bias=1.5 snapshot_bias=0\n";
    return 0;
}
