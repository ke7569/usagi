#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common/stream/HardwareTimestamping.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ifaddrs.h>
#include <linux/ethtool.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace deepwin_market_data {
namespace timestamping {
namespace {

class Socket {
public:
    Socket() : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
        if (fd_ < 0) throw std::runtime_error(errno_message("create timestamp ioctl socket"));
    }
    ~Socket() {
        if (fd_ >= 0) ::close(fd_);
    }
    int fd() const { return fd_; }

private:
    static std::string errno_message(const char* operation) {
        return std::string(operation) + ": " + std::strerror(errno);
    }
    int fd_;
    Socket(const Socket&);
    Socket& operator=(const Socket&);
};

std::string errno_message(const std::string& operation, int value = errno) {
    return operation + ": " + std::strerror(value);
}

void interface_request(struct ifreq* request, const std::string& interface_name) {
    if (!request || interface_name.empty() || interface_name.size() >= IFNAMSIZ)
        throw std::runtime_error("invalid network interface name");
    std::memset(request, 0, sizeof(*request));
    std::memcpy(request->ifr_name, interface_name.c_str(), interface_name.size());
}

void ethtool_timestamp_info(int fd, const std::string& interface_name,
                            struct ethtool_ts_info* output) {
    if (!output) throw std::runtime_error("null ethtool timestamp info output");
    std::memset(output, 0, sizeof(*output));
    output->cmd = ETHTOOL_GET_TS_INFO;
    struct ifreq request;
    interface_request(&request, interface_name);
    request.ifr_data = reinterpret_cast<char*>(output);
    if (::ioctl(fd, SIOCETHTOOL, &request) != 0)
        throw std::runtime_error(errno_message(
            "ETHTOOL_GET_TS_INFO for " + interface_name));
}

bool hwtstamp_query_unsupported(int value) {
    return value == EOPNOTSUPP || value == ENOTTY || value == EINVAL || value == ENOSYS;
}

bool get_hwtstamp_config(int fd, const std::string& interface_name,
                         struct hwtstamp_config* output, int* error_number) {
    if (!output) throw std::runtime_error("null hwtstamp config output");
    std::memset(output, 0, sizeof(*output));
    struct ifreq request;
    interface_request(&request, interface_name);
    request.ifr_data = reinterpret_cast<char*>(output);
    if (::ioctl(fd, SIOCGHWTSTAMP, &request) == 0) return true;
    if (error_number) *error_number = errno;
    return false;
}

void set_hwtstamp_config(int fd, const std::string& interface_name,
                         struct hwtstamp_config* config) {
    struct ifreq request;
    interface_request(&request, interface_name);
    request.ifr_data = reinterpret_cast<char*>(config);
    if (::ioctl(fd, SIOCSHWTSTAMP, &request) != 0)
        throw std::runtime_error(errno_message(
            "SIOCSHWTSTAMP for " + interface_name));
}

void require_interface_ip(const std::string& interface_name,
                          const std::string& interface_ip) {
    in_addr wanted;
    if (interface_ip.empty() || ::inet_pton(AF_INET, interface_ip.c_str(), &wanted) != 1)
        throw std::runtime_error("invalid IPv4 address for " + interface_name + ": " + interface_ip);
    struct ifaddrs* addresses = 0;
    if (::getifaddrs(&addresses) != 0)
        throw std::runtime_error(errno_message("getifaddrs"));
    bool found = false;
    for (struct ifaddrs* item = addresses; item; item = item->ifa_next) {
        if (!item->ifa_name || interface_name != item->ifa_name || !item->ifa_addr ||
            item->ifa_addr->sa_family != AF_INET)
            continue;
        const struct sockaddr_in* address =
            reinterpret_cast<const struct sockaddr_in*>(item->ifa_addr);
        if (address->sin_addr.s_addr == wanted.s_addr) {
            found = true;
            break;
        }
    }
    ::freeifaddrs(addresses);
    if (!found) {
        throw std::runtime_error("IPv4 address " + interface_ip +
                                 " is not assigned to interface " + interface_name);
    }
}

bool has_index_bit(unsigned value, unsigned bit) {
    return bit < sizeof(unsigned) * 8U && (value & (1U << bit)) != 0U;
}

void require_capability(const DeviceInfo& device, unsigned mask, const char* name) {
    if ((device.capabilities & mask) != mask)
        throw std::runtime_error(std::string("interface lacks ") + name +
                                 " timestamp capability");
}

void require_filter(const DeviceInfo& device, unsigned filter, const char* name) {
    if (!has_index_bit(device.rx_filters, filter))
        throw std::runtime_error(std::string("interface lacks ") + name +
                                 " hardware receive filter");
}

}  // namespace

DeviceInfo inspect_device(const std::string& interface_name) {
    if (interface_name.empty() || interface_name.size() >= IFNAMSIZ)
        throw std::runtime_error("invalid network interface name");
    if (::if_nametoindex(interface_name.c_str()) == 0)
        throw std::runtime_error(errno_message("find interface " + interface_name));

    Socket socket;
    struct ethtool_ts_info info;
    ethtool_timestamp_info(socket.fd(), interface_name, &info);
    DeviceInfo result;
    result.phc_index = info.phc_index;
    result.capabilities = info.so_timestamping;
    result.rx_filters = info.rx_filters;

    struct hwtstamp_config config;
    int get_error = 0;
    if (get_hwtstamp_config(socket.fd(), interface_name, &config, &get_error)) {
        result.config_known = true;
        result.tx_type = config.tx_type;
        result.rx_filter = config.rx_filter;
    } else if (!hwtstamp_query_unsupported(get_error)) {
        throw std::runtime_error(errno_message(
            "SIOCGHWTSTAMP for " + interface_name, get_error));
    }
    return result;
}

int require_hardware_receive(const std::string& interface_name,
                             const std::string& interface_ip) {
    require_interface_ip(interface_name, interface_ip);
    const DeviceInfo device = inspect_device(interface_name);
    require_capability(device, SOF_TIMESTAMPING_RX_HARDWARE, "RX hardware");
    require_capability(device, SOF_TIMESTAMPING_RAW_HARDWARE, "RAW hardware");
    require_filter(device, HWTSTAMP_FILTER_ALL, "ALL");
    if (device.phc_index < 0)
        throw std::runtime_error("interface has no associated PTP hardware clock");
    if (!device.config_known)
        throw std::runtime_error("SIOCGHWTSTAMP is unavailable; driver cannot verify the "
                                 "current hardware receive filter");
    if (device.rx_filter != HWTSTAMP_FILTER_ALL) {
        std::ostringstream message;
        message << "interface RX timestamp filter is " << device.rx_filter
                << ", expected HWTSTAMP_FILTER_ALL; use the explicit enable-rx-all tool";
        throw std::runtime_error(message.str());
    }
    return device.phc_index;
}

void enable_receive_all(const std::string& interface_name) {
    if (interface_name.empty() || interface_name.size() >= IFNAMSIZ)
        throw std::runtime_error("invalid network interface name");
    if (::if_nametoindex(interface_name.c_str()) == 0)
        throw std::runtime_error(errno_message("find interface " + interface_name));

    Socket socket;
    struct ethtool_ts_info info;
    ethtool_timestamp_info(socket.fd(), interface_name, &info);
    if (!has_index_bit(info.rx_filters, HWTSTAMP_FILTER_ALL))
        throw std::runtime_error("interface does not advertise HWTSTAMP_FILTER_ALL");

    struct hwtstamp_config current;
    int get_error = 0;
    if (!get_hwtstamp_config(socket.fd(), interface_name, &current, &get_error))
        throw std::runtime_error(errno_message(
            "SIOCGHWTSTAMP for " + interface_name + " (device unchanged)", get_error));
    const int saved_flags = current.flags;
    const int saved_tx_type = current.tx_type;
    current.rx_filter = HWTSTAMP_FILTER_ALL;
    set_hwtstamp_config(socket.fd(), interface_name, &current);
    if (current.flags != saved_flags || current.tx_type != saved_tx_type ||
        current.rx_filter != HWTSTAMP_FILTER_ALL) {
        std::ostringstream message;
        message << "SIOCSHWTSTAMP did not preserve TX/config or return RX ALL"
                << " (flags=" << current.flags << ", tx_type=" << current.tx_type
                << ", rx_filter=" << current.rx_filter << ')';
        throw std::runtime_error(message.str());
    }

    struct hwtstamp_config verified;
    int verify_error = 0;
    if (!get_hwtstamp_config(socket.fd(), interface_name, &verified, &verify_error))
        throw std::runtime_error(errno_message(
            "verify SIOCGHWTSTAMP for " + interface_name, verify_error));
    if (verified.flags != saved_flags || verified.tx_type != saved_tx_type ||
        verified.rx_filter != HWTSTAMP_FILTER_ALL)
        throw std::runtime_error("SIOCSHWTSTAMP verification did not report preserved TX and RX ALL");
}

std::string clock_domain(int phc_index) {
    if (phc_index < 0) return std::string();
    return std::string("/dev/ptp") + std::to_string(phc_index);
}

}  // namespace timestamping
}  // namespace deepwin_market_data
