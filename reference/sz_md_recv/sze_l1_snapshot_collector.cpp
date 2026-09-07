#define _GNU_SOURCE

#include "sze/market_data/SZEProtocol.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <getopt.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <climits>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

struct Config {
    std::string group = "239.35.80.1";
    std::string iface_ip = "11.11.11.11";
    std::string ifname;
    std::string output = "sze_l1_snapshots.csv";
    int port = 37100;
    int bind_port = 0;
    int seconds = 0;
    int rcvbuf_mb = 256;
};

void on_signal(int) { g_stop = 1; }

void usage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "  --group IP        multicast group (default 239.35.80.1)\n"
        << "  --port N          multicast port (default 37100)\n"
        << "  --iface-ip IP     local market-data IP (default 11.11.11.11)\n"
        << "  --ifname NAME     optional interface name\n"
        << "  --bind-port N     local UDP bind port, 0 means --port\n"
        << "  --seconds N       stop after N seconds, 0 means Ctrl-C\n"
        << "  --rcvbuf-mb N     receive buffer size (default 256)\n"
        << "  --output PATH     CSV output path (default sze_l1_snapshots.csv)\n";
}

bool parse_args(int argc, char** argv, Config* cfg)
{
    enum { GROUP = 1000, PORT, IFACE_IP, IFNAME, BIND_PORT, SECONDS, RCVBUF, OUTPUT };
    const option options[] = {
        {"group", required_argument, nullptr, GROUP},
        {"port", required_argument, nullptr, PORT},
        {"iface-ip", required_argument, nullptr, IFACE_IP},
        {"ifname", required_argument, nullptr, IFNAME},
        {"bind-port", required_argument, nullptr, BIND_PORT},
        {"seconds", required_argument, nullptr, SECONDS},
        {"rcvbuf-mb", required_argument, nullptr, RCVBUF},
        {"output", required_argument, nullptr, OUTPUT},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    for (;;) {
        const int c = getopt_long(argc, argv, "h", options, nullptr);
        if (c == -1) break;
        switch (c) {
        case GROUP: cfg->group = optarg; break;
        case PORT: cfg->port = std::atoi(optarg); break;
        case IFACE_IP: cfg->iface_ip = optarg; break;
        case IFNAME: cfg->ifname = optarg; break;
        case BIND_PORT: cfg->bind_port = std::atoi(optarg); break;
        case SECONDS: cfg->seconds = std::atoi(optarg); break;
        case RCVBUF: cfg->rcvbuf_mb = std::atoi(optarg); break;
        case OUTPUT: cfg->output = optarg; break;
        case 'h': usage(argv[0]); return false;
        default: usage(argv[0]); return false;
        }
    }
    if (cfg->port < 1 || cfg->port > 65535 || cfg->bind_port < 0 || cfg->bind_port > 65535 ||
        cfg->seconds < 0 || cfg->rcvbuf_mb < 1) {
        std::cerr << "invalid numeric option\n";
        return false;
    }
    return true;
}

int open_socket(const Config& cfg)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::perror("socket"); return -1; }
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    const std::int64_t requested_bytes = static_cast<std::int64_t>(cfg.rcvbuf_mb) * 1024 * 1024;
    if (requested_bytes > static_cast<std::int64_t>(INT_MAX)) {
        std::cerr << "--rcvbuf-mb is too large for socket option\n";
        ::close(fd);
        return -1;
    }
    const int bytes = static_cast<int>(requested_bytes);
#ifdef SO_RCVBUFFORCE
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &bytes, sizeof(bytes)) != 0)
#endif
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));

    if (!cfg.ifname.empty() &&
        ::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, cfg.ifname.c_str(),
                     cfg.ifname.size() + 1) != 0) {
        std::perror("SO_BINDTODEVICE");
        ::close(fd);
        return -1;
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(static_cast<uint16_t>(cfg.bind_port ? cfg.bind_port : cfg.port));
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        std::perror("bind");
        ::close(fd);
        return -1;
    }

    ip_mreqn membership{};
    if (::inet_pton(AF_INET, cfg.group.c_str(), &membership.imr_multiaddr) != 1 ||
        ::inet_pton(AF_INET, cfg.iface_ip.c_str(), &membership.imr_address) != 1) {
        std::cerr << "invalid multicast or interface address\n";
        ::close(fd);
        return -1;
    }
    if (!cfg.ifname.empty()) {
        membership.imr_ifindex = static_cast<int>(::if_nametoindex(cfg.ifname.c_str()));
        if (membership.imr_ifindex == 0) {
            std::cerr << "unknown interface: " << cfg.ifname << "\n";
            ::close(fd);
            return -1;
        }
    }
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
        std::perror("IP_ADD_MEMBERSHIP");
        ::close(fd);
        return -1;
    }
    return fd;
}

void write_header(std::ofstream& out)
{
    out << "recv_realtime_ns,feed_sequence,channel_sequence,channel_num,"
           "symbol,trading_day,update_time,update_millisec,last_price,pre_close_price,"
           "open_price,high_price,low_price,close_price,upper_limit,lower_limit,"
           "volume,turnover,open_interest";
    for (int level = 1; level <= 10; ++level) {
        out << ",bid_price_" << level << ",bid_volume_" << level
            << ",ask_price_" << level << ",ask_volume_" << level;
    }
    out << '\n';
}

void write_snapshot(std::ofstream& out, uint64_t recv_ns,
                    const sze_md::SzeHpfSnapshot& wire,
                    const LFMarketDataField& value)
{
    out << recv_ns << ',' << wire.head.sequence << ',' << wire.head.sequence_num << ','
        << wire.head.channel_num << ',' << value.InstrumentID << ',' << value.TradingDay << ','
        << value.UpdateTime << ',' << value.UpdateMillisec << ','
        << std::setprecision(17) << value.LastPrice << ',' << value.PreClosePrice << ','
        << value.OpenPrice << ',' << value.HighestPrice << ',' << value.LowestPrice << ','
        << value.ClosePrice << ',' << value.UpperLimitPrice << ',' << value.LowerLimitPrice << ','
        << value.Volume << ',' << value.Turnover << ',' << value.OpenInterest;
    for (int level = 0; level < 10; ++level) {
        out << ',' << value.aBidPrice[level] << ',' << value.aBidVolume[level]
            << ',' << value.aAskPrice[level] << ',' << value.aAskVolume[level];
    }
    out << '\n';
}

uint64_t realtime_ns()
{
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

bool decode_source88_snapshot(const void* record, std::size_t length,
                              LFMarketDataField* value)
{
    if (length != sze_md::kSnapshotRecordSize) return false;
    auto* wire = const_cast<sze_md::SzeHpfSnapshot*>(
        static_cast<const sze_md::SzeHpfSnapshot*>(record));
    const std::uint64_t sequence_num = wire->head.sequence_num;
    if (sequence_num == 0) wire->head.sequence_num = 1;
    const bool decoded = sze_md::decode_snapshot(wire, length, value);
    wire->head.sequence_num = sequence_num;
    return decoded;
}

}  // namespace

int main(int argc, char** argv)
{
    Config cfg;
    if (!parse_args(argc, argv, &cfg)) return 2;
    std::ofstream out(cfg.output.c_str(), std::ios::out | std::ios::trunc);
    if (!out) { std::cerr << "cannot open output: " << cfg.output << '\n'; return 1; }
    write_header(out);

    const int fd = open_socket(cfg);
    if (fd < 0) return 1;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::cerr << "listening group=" << cfg.group << " port=" << cfg.port
              << " iface_ip=" << cfg.iface_ip << " output=" << cfg.output << '\n';

    std::array<unsigned char, 65536> packet{};
    uint64_t packets = 0, snapshots = 0, malformed = 0, ignored = 0, bytes = 0;
    const uint64_t started = realtime_ns();
    while (!g_stop) {
        if (cfg.seconds > 0 && realtime_ns() - started >= static_cast<uint64_t>(cfg.seconds) * 1000000000ULL)
            break;
        pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 1000);
        if (ready < 0) { if (errno == EINTR) continue; std::perror("poll"); break; }
        if (ready == 0) continue;
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::cerr << "socket poll error, revents=" << pfd.revents << '\n';
            break;
        }
        if ((pfd.revents & POLLIN) == 0) continue;
        sockaddr_in source{};
        socklen_t source_len = sizeof(source);
        const ssize_t length = ::recvfrom(fd, packet.data(), packet.size(), 0,
                                          reinterpret_cast<sockaddr*>(&source), &source_len);
        if (length < 0) { if (errno == EINTR) continue; std::perror("recvfrom"); break; }
        ++packets;
        bytes += static_cast<uint64_t>(length);
        std::size_t offset = 0;
        bool packet_malformed = false;
        while (offset + 9 <= static_cast<std::size_t>(length)) {
            const auto* head = reinterpret_cast<const sze_md::SzeHpfHead*>(packet.data() + offset);
            const std::size_t record_size = sze_md::wire_record_size(head->message_type);
            if (record_size == 0 || offset + record_size > static_cast<std::size_t>(length)) {
                packet_malformed = true;
                break;
            }
            if (head->message_type == sze_md::kSnapshotMessage) {
                LFMarketDataField value{};
                const auto* wire = reinterpret_cast<const sze_md::SzeHpfSnapshot*>(packet.data() + offset);
                if (!decode_source88_snapshot(wire, record_size, &value)) {
                    ++malformed;
                } else {
                    write_snapshot(out, realtime_ns(), *wire, value);
                    ++snapshots;
                }
            } else {
                ++ignored;
            }
            offset += record_size;
        }
        if (offset != static_cast<std::size_t>(length)) packet_malformed = true;
        if (packet_malformed) ++malformed;
    }
    out.flush();
    ::close(fd);
    std::cerr << "done packets=" << packets << " bytes=" << bytes
              << " snapshots=" << snapshots << " ignored=" << ignored
              << " malformed=" << malformed << '\n';
    return 0;
}
