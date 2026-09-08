#ifndef USAGI_HARDWARE_TIMESTAMPING_H
#define USAGI_HARDWARE_TIMESTAMPING_H

#include <string>

namespace deepwin_market_data {
namespace timestamping {

// The bit fields use the values reported by ETHTOOL_GET_TS_INFO.  tx_type and
// rx_filter are the current SIOCGHWTSTAMP configuration, or -1 when the
// driver does not expose that configuration query.
struct DeviceInfo {
    DeviceInfo()
        : phc_index(-1), capabilities(0U), rx_filters(0U), tx_type(-1),
          rx_filter(-1), config_known(false) {}

    int phc_index;
    unsigned capabilities;
    unsigned rx_filters;
    int tx_type;
    int rx_filter;
    bool config_known;
};

// Throws std::runtime_error with the interface and kernel error on an
// ethtool/interface query failure. An unsupported SIOCGHWTSTAMP is represented
// by config_known=false; callers requiring hardware receive must reject it.
DeviceInfo inspect_device(const std::string& interface_name);

// Validates that interface_ip belongs to interface_name, that the NIC exposes
// RX/RAW hardware timestamping and HWTSTAMP_FILTER_ALL, and that the driver
// currently reports RX ALL enabled. Returns the associated PHC index.
int require_hardware_receive(const std::string& interface_name,
                             const std::string& interface_ip);

// Explicit device-changing operation. Reads and preserves the current TX type
// and flags, requests HWTSTAMP_FILTER_ALL, and verifies the driver result.
// This function is intentionally never called by inspect_device() or require_*
// and may require root/CAP_NET_ADMIN.
void enable_receive_all(const std::string& interface_name);

std::string clock_domain(int phc_index);

}  // namespace timestamping
}  // namespace deepwin_market_data

#endif  // USAGI_HARDWARE_TIMESTAMPING_H
