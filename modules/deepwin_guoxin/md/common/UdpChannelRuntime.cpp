#include "UdpChannelRuntime.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <sched.h>

#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace deepwin_market_data {
namespace {

const std::size_t kMaxDatagram = 65536U;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool valid_ipv4(const std::string& value, in_addr* address) {
    return !value.empty() && ::inet_pton(AF_INET, value.c_str(), address) == 1;
}

int open_socket(const ChannelSpec& channel, std::string* error) {
    in_addr group_address;
    if (!valid_ipv4(channel.group, &group_address)) {
        if (error) *error = "invalid IPv4 multicast/unicast group for channel " + channel.name;
        return -1;
    }
    if (channel.port <= 0 || channel.port > 65535) {
        if (error) *error = "invalid UDP port for channel " + channel.name;
        return -1;
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (error) *error = "socket failed for channel " + channel.name + ": " + std::strerror(errno);
        return -1;
    }
    const int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in bind_address;
    std::memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(static_cast<unsigned short>(channel.port));
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address)) < 0) {
        if (error) *error = "bind failed for channel " + channel.name + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }

    // Loopback unicast is used by the offline test. Production groups are
    // multicast and require an explicit membership join.
    if (channel.group != "127.0.0.1") {
        ip_mreq membership;
        std::memset(&membership, 0, sizeof(membership));
        membership.imr_multiaddr = group_address;
        if (channel.interface_ip.empty() || channel.interface_ip == "0.0.0.0") {
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
        } else if (!valid_ipv4(channel.interface_ip, &membership.imr_interface)) {
            if (error) *error = "invalid interface IPv4 address for channel " + channel.name;
            ::close(fd);
            return -1;
        }
        if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &membership, sizeof(membership)) < 0) {
            if (error) *error = "IP_ADD_MEMBERSHIP failed for channel " + channel.name +
                ": " + std::strerror(errno);
            ::close(fd);
            return -1;
        }
    }

    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef SO_TIMESTAMPNS
    const int timestamp_ns = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS,
                       &timestamp_ns, sizeof(timestamp_ns));
#endif
    return fd;
}

}  // namespace

struct UdpChannelRuntime::Impl {
    std::atomic<bool> running;
    std::mutex error_mutex;
    std::string error;

    Impl() : running(false) {}

    void set_error(const std::string& value) {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (error.empty()) error = value;
    }

    void worker(const ChannelSpec& channel, const DatagramCallback& callback,
                long duration_ms) {
        if (channel.receive_cpu != -1) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            if (channel.receive_cpu < 0 || channel.receive_cpu >= CPU_SETSIZE) {
                set_error("invalid receive CPU for channel " + channel.name);
                running.store(false);
                return;
            }
            CPU_SET(channel.receive_cpu, &mask);
            if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
                set_error("cannot bind receive CPU for channel " + channel.name + ": " + std::strerror(errno));
                running.store(false);
                return;
            }
        }
        std::string open_error;
        const int fd = open_socket(channel, &open_error);
        if (fd < 0) {
            set_error(open_error);
            running.store(false);
            return;
        }
        const std::int64_t deadline = duration_ms > 0
            ? now_ns() + static_cast<std::int64_t>(duration_ms) * 1000000LL
            : std::numeric_limits<std::int64_t>::max();
        std::vector<unsigned char> packet(kMaxDatagram);
        while (running.load()) {
            sockaddr_in source;
            std::memset(&source, 0, sizeof(source));
            socklen_t source_length = sizeof(source);
            char control[CMSG_SPACE(sizeof(timespec))];
            std::memset(control, 0, sizeof(control));
            iovec payload;
            payload.iov_base = packet.data();
            payload.iov_len = packet.size();
            msghdr message;
            std::memset(&message, 0, sizeof(message));
            message.msg_name = &source;
            message.msg_namelen = source_length;
            message.msg_iov = &payload;
            message.msg_iovlen = 1;
            message.msg_control = control;
            message.msg_controllen = sizeof(control);
            const ssize_t length = ::recvmsg(fd, &message, 0);
            if (length < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (now_ns() >= deadline) break;
                    continue;
                }
                set_error("recvfrom failed for channel " + channel.name + ": " + std::strerror(errno));
                break;
            }
            if (now_ns() >= deadline && duration_ms > 0) break;
            std::int64_t receive_ns = now_ns();
#ifdef SO_TIMESTAMPNS
            for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != 0;
                 header = CMSG_NXTHDR(&message, header)) {
                if (header->cmsg_level == SOL_SOCKET &&
                    header->cmsg_type == SCM_TIMESTAMPNS &&
                    header->cmsg_len >= CMSG_LEN(sizeof(timespec))) {
                    const timespec* timestamp =
                        reinterpret_cast<const timespec*>(CMSG_DATA(header));
                    receive_ns = static_cast<std::int64_t>(timestamp->tv_sec) * 1000000000LL +
                        static_cast<std::int64_t>(timestamp->tv_nsec);
                    break;
                }
            }
#endif
            char source_ip[INET_ADDRSTRLEN] = {};
            if (::inet_ntop(AF_INET, &source.sin_addr, source_ip, sizeof(source_ip)) == 0) {
                source_ip[0] = '\0';
            }
            Datagram datagram;
            datagram.channel = channel.name;
            datagram.source_ip = source_ip;
            datagram.source_port = ntohs(source.sin_port);
            datagram.receive_ns = receive_ns;
            datagram.monotonic_ns = monotonic_ns();
            datagram.data = packet.data();
            datagram.size = static_cast<std::size_t>(length);
            callback(datagram);
        }
        ::close(fd);
    }
};

UdpChannelRuntime::UdpChannelRuntime() : impl_(new Impl()) {}
UdpChannelRuntime::~UdpChannelRuntime() { impl_->running.store(false); delete impl_; }

void UdpChannelRuntime::stop() { impl_->running.store(false); }

bool UdpChannelRuntime::run(const std::vector<ChannelSpec>& channels,
                            const DatagramCallback& callback,
                            long duration_ms,
                            std::string* error) {
    if (error) error->clear();
    if (channels.empty()) {
        if (error) *error = "at least one UDP channel is required";
        return false;
    }
    if (!callback) {
        if (error) *error = "UDP channel callback is required";
        return false;
    }
    if (duration_ms < 0) {
        if (error) *error = "duration_ms must be non-negative";
        return false;
    }
    impl_->error.clear();
    impl_->running.store(true);
    std::vector<std::thread> workers;
    workers.reserve(channels.size());
    for (const ChannelSpec& channel : channels) {
        workers.emplace_back(&Impl::worker, impl_, channel, callback, duration_ms);
    }
    for (std::thread& worker : workers) worker.join();
    impl_->running.store(false);
    if (!impl_->error.empty()) {
        if (error) *error = impl_->error;
        return false;
    }
    return true;
}

}  // namespace deepwin_market_data
