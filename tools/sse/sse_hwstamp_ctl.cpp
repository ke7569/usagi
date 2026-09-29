#include "common/stream/HardwareTimestamping.h"
#include "third_party/nlohmann/json.hpp"

#include <linux/net_tstamp.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

typedef nlohmann::json Json;

void print_usage() {
    std::cerr << "usage: sse_hwstamp_ctl INTERFACE [show|enable-rx-all]\n";
}

Json device_json(const std::string& interface_name,
                 const deepwin_market_data::timestamping::DeviceInfo& device) {
    Json output;
    output["interface"] = interface_name;
    output["phc_index"] = device.phc_index;
    output["clock_domain"] = deepwin_market_data::timestamping::clock_domain(device.phc_index);
    output["capabilities"] = device.capabilities;
    output["rx_filters"] = device.rx_filters;
    output["tx_type"] = device.tx_type;
    output["rx_filter"] = device.rx_filter;
    output["config_known"] = device.config_known;
    output["rx_hardware_supported"] =
        (device.capabilities & SOF_TIMESTAMPING_RX_HARDWARE) != 0U;
    output["raw_hardware_supported"] =
        (device.capabilities & SOF_TIMESTAMPING_RAW_HARDWARE) != 0U;
    output["rx_filter_all_supported"] =
        (device.rx_filters & (1U << HWTSTAMP_FILTER_ALL)) != 0U;
    output["rx_filter_all_enabled"] =
        device.config_known && device.rx_filter == HWTSTAMP_FILTER_ALL;
    return output;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        print_usage();
        return 2;
    }
    const std::string interface_name = argv[1];
    const std::string operation = argc == 3 ? argv[2] : "show";
    if (operation != "show" && operation != "enable-rx-all") {
        print_usage();
        return 2;
    }
    try {
        if (operation == "enable-rx-all")
            deepwin_market_data::timestamping::enable_receive_all(interface_name);
        const deepwin_market_data::timestamping::DeviceInfo device =
            deepwin_market_data::timestamping::inspect_device(interface_name);
        Json output = device_json(interface_name, device);
        output["operation"] = operation;
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sse_hwstamp_ctl: " << error.what() << '\n';
        return 1;
    }
}
