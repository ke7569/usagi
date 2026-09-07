#include "adapters/deepwin/legacy/wc_strategy.h"
#include "IControlCenter.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

extern "C" IWCStrategy* get_obj(
    kungfu::yijinjing::IControlCenter* cc,
    const std::string& cfg_name);

namespace {

bool expect_factory_rejection(const char* label, const char* document) {
    std::ostringstream path_builder;
    path_builder << "/tmp/t0_sze_get_obj_" << label << '_' << getpid() << ".json";
    const std::string path = path_builder.str();
    {
        std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            std::cerr << "failed to create test config: " << path << std::endl;
            return false;
        }
        output << document;
    }

    IWCStrategy* strategy = get_obj(0, path);
    std::remove(path.c_str());
    if (strategy != 0) {
        std::cerr << "factory accepted rejected market case: " << label << std::endl;
        delete strategy;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!expect_factory_rejection("sh", "{\"market\":\"SH\"}\n")) {
        return 1;
    }
    if (!expect_factory_rejection("missing", "{}\n")) {
        return 2;
    }
    if (!expect_factory_rejection("numeric", "{\"market\":89}\n")) {
        return 3;
    }
    if (!expect_factory_rejection("other", "{\"market\":\"BJ\"}\n")) {
        return 4;
    }
    if (!expect_factory_rejection(
            "hp_missing_model_path",
            "{\"market\":\"SZ\",\"sz_orderbook_mode\":\"hp-shadow\"}\n")) {
        return 5;
    }
    if (!expect_factory_rejection(
            "hp_model_alias",
            "{\"market\":\"SZ\",\"sz_orderbook_mode\":\"hp-shadow\","
            "\"model_path\":\"/tmp/mix.bin\","
            "\"mix153060_model_artifact\":\"/tmp/other.bin\"}\n")) {
        return 6;
    }
    std::cout << "sze_get_obj_rejection_test: PASS" << std::endl;
    return 0;
}
