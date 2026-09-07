#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "adapters/deepwin/md/MDEngineSZEL1.h"
#include "sze/market_data/SZEProtocol.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

WC_NAMESPACE_START

namespace {
const short kSzeL1SourceId = 90;
const std::size_t kMaxPacketSize = 65536;

template <typename T>
T json_value_or(const json& value, const char* key, const T& fallback) {
    if (value.find(key) == value.end()) return fallback;
    try { return value[key].get<T>(); } catch (...) { return fallback; }
}
}

MDEngineSZEL1::MDEngineSZEL1() : IMDEngine(kSzeL1SourceId) {
    logger = yijinjing::KfLog::getLogger("MdEngine.SZE.L1");
}

MDEngineSZEL1::~MDEngineSZEL1() { release_api(); }

void MDEngineSZEL1::load(const json& config) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load()) throw std::runtime_error("cannot reload SZE L1 while running");
    channels_.clear();
    filter_enabled_ = json_value_or<bool>(config, "use_subscribe_filter", false);
    subscribe_all_ = json_value_or<bool>(config, "subscribe_all", !filter_enabled_);
    const json& input = config.find("channels") != config.end() ? config["channels"] : config;
    const std::size_t count = input.is_array() ? input.size() : 1U;
    for (std::size_t index = 0; index < count; ++index) {
        const json& value = input.is_array() ? input[index] : input;
        Channel channel;
        channel.multicast_ip = json_value_or<std::string>(value, "multicast_ip",
            json_value_or<std::string>(value, "group", std::string()));
        channel.iface_ip = json_value_or<std::string>(value, "iface_ip",
            json_value_or<std::string>(value, "local_ip", std::string()));
        channel.port = json_value_or<int>(value, "port", 0);
        channel.cpu = json_value_or<int>(value, "cpu", -1);
        channel.rcvbuf_mb = json_value_or<int>(value, "rcvbuf_mb", 64);
        if (channel.multicast_ip.empty() || channel.port <= 0 || channel.port > 65535 ||
            channel.rcvbuf_mb < 1 || channel.rcvbuf_mb > 2048) {
            throw std::runtime_error("SZE L1 requires valid multicast_ip, port, and rcvbuf_mb");
        }
        channels_.push_back(channel);
    }
    KF_LOG_INFO(logger, "[load] SZE L1 snapshot receiver source=" << kSzeL1SourceId
        << " channels=" << channels_.size() << " subscribe_all=" << (subscribe_all_ ? 1 : 0));
}

void MDEngineSZEL1::connect(long) { connected_ = !channels_.empty(); }

void MDEngineSZEL1::login(long) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (logged_in_) return;
    if (!connected_) connect(0);
    if (!connected_) throw std::runtime_error("no SZE L1 channel configured");
    sockets_.clear();
    for (const Channel& channel : channels_) {
        const int fd = open_socket(channel);
        if (fd < 0) { close_sockets(); throw std::runtime_error("failed to open SZE L1 multicast socket"); }
        sockets_.push_back(fd);
    }
    running_.store(true);
    workers_.reserve(sockets_.size());
    for (std::size_t index = 0; index < sockets_.size(); ++index)
        workers_.emplace_back(&MDEngineSZEL1::worker_loop, this, index);
    logged_in_ = true;
    KF_LOG_INFO(logger, "[login] SZE L1 snapshot receiver started channels=" << workers_.size());
}

void MDEngineSZEL1::logout() { release_api(); }

void MDEngineSZEL1::release_api() {
    running_.store(false);
    for (std::thread& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
    close_sockets();
    if (logged_in_) KF_LOG_INFO(logger, "[shutdown] SZE L1 packets=" << packets_.load()
        << " snapshots=" << snapshots_.load() << " filtered=" << filtered_.load()
        << " malformed=" << malformed_.load() << " ignored=" << ignored_.load());
    connected_ = false;
    logged_in_ = false;
}

void MDEngineSZEL1::subscribeMarketData(const std::vector<std::string>& instruments,
                                        const std::vector<std::string>&) { update_filter(instruments); }
void MDEngineSZEL1::subscribeL2MD(const std::vector<std::string>&, const std::vector<std::string>&) {}
void MDEngineSZEL1::subscribeOrderTrade(const std::vector<std::string>&, const std::vector<std::string>&) {}

void MDEngineSZEL1::update_filter(const std::vector<std::string>& instruments) {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    symbols_.clear();
    for (const std::string& instrument : instruments) {
        const std::string normalized = normalize_symbol(instrument);
        if (!normalized.empty()) symbols_.insert(normalized);
    }
    if (!symbols_.empty()) subscribe_all_ = false;
}

bool MDEngineSZEL1::should_forward(const char* instrument) const {
    if (!filter_enabled_ || subscribe_all_) return true;
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    return symbols_.find(normalize_symbol(instrument == 0 ? "" : instrument)) != symbols_.end();
}

std::string MDEngineSZEL1::normalize_symbol(const std::string& value) {
    const std::size_t dot = value.find('.');
    return value.substr(0, dot == std::string::npos ? value.size() : dot);
}

void MDEngineSZEL1::set_cpu_affinity(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    (void)::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
}

int MDEngineSZEL1::open_socket(const Channel& channel) const {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    const int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const int rcvbuf = channel.rcvbuf_mb * 1024 * 1024;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    sockaddr_in address; std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; address.sin_port = htons(static_cast<uint16_t>(channel.port));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { ::close(fd); return -1; }
    ip_mreq membership; std::memset(&membership, 0, sizeof(membership));
    membership.imr_multiaddr.s_addr = ::inet_addr(channel.multicast_ip.c_str());
    membership.imr_interface.s_addr = channel.iface_ip.empty() ? htonl(INADDR_ANY) : ::inet_addr(channel.iface_ip.c_str());
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) { ::close(fd); return -1; }
    return fd;
}

void MDEngineSZEL1::close_sockets() { for (int fd : sockets_) if (fd >= 0) ::close(fd); sockets_.clear(); }

void MDEngineSZEL1::worker_loop(std::size_t index) {
    set_cpu_affinity(channels_[index].cpu);
    std::vector<unsigned char> packet(kMaxPacketSize);
    while (running_.load()) {
        const ssize_t length = ::recv(sockets_[index], packet.data(), packet.size(), 0);
        if (length < 0) { if (errno == EINTR) continue; if (running_.load()) ++malformed_; continue; }
        ++packets_;
        std::size_t offset = 0;
        while (offset + sizeof(sze_md::SzeHpfHead) <= static_cast<std::size_t>(length)) {
            const sze_md::SzeHpfHead* head = reinterpret_cast<const sze_md::SzeHpfHead*>(packet.data() + offset);
            const std::size_t record_size = sze_md::wire_record_size(head->message_type);
            if (record_size == 0U || offset + record_size > static_cast<std::size_t>(length)) { ++malformed_; break; }
            if (head->message_type == sze_md::kSnapshotMessage) {
                LFMarketDataField snapshot = {};
                sze_md::SzeHpfSnapshot* wire =
                    reinterpret_cast<sze_md::SzeHpfSnapshot*>(packet.data() + offset);
                const std::uint64_t sequence_num = wire->head.sequence_num;
                if (sequence_num == 0U) {
                    wire->head.sequence_num = 1U;
                }
                const bool decoded =
                    sze_md::decode_snapshot(wire, record_size, &snapshot);
                wire->head.sequence_num = sequence_num;
                if (!decoded) ++malformed_;
                else if (!should_forward(snapshot.InstrumentID)) ++filtered_;
                else { ++snapshots_; on_market_data(&snapshot); }
            } else ++ignored_;
            offset += record_size;
        }
        if (offset != static_cast<std::size_t>(length)) ++malformed_;
    }
}

extern "C" IMDEngine* get_obj(IControlCenter* pcc) {
    return kungfu::wingchun::md_get_obj<MDEngineSZEL1>(pcc, "sze_l1");
}

WC_NAMESPACE_END
