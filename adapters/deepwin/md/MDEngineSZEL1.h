#ifndef MDENGINE_SZEL1_H
#define MDENGINE_SZEL1_H

#include "IMDEngine.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

WC_NAMESPACE_START

class MDEngineSZEL1 : public IMDEngine {
public:
    MDEngineSZEL1();
    ~MDEngineSZEL1() override;

    void load(const json& j_config) override;
    void connect(long timeout_nsec) override;
    void login(long timeout_nsec) override;
    void logout() override;
    void release_api() override;
    void subscribeMarketData(const std::vector<std::string>& instruments,
                             const std::vector<std::string>& markets) override;
    void subscribeL2MD(const std::vector<std::string>& instruments,
                       const std::vector<std::string>& markets) override;
    void subscribeOrderTrade(const std::vector<std::string>& instruments,
                             const std::vector<std::string>& markets) override;
    bool is_connected() const override { return connected_; }
    bool is_logged_in() const override { return logged_in_; }
    std::string name() const override { return "MDEngineSZEL1"; }

private:
    struct Channel {
        std::string multicast_ip;
        std::string iface_ip;
        int port = 0;
        int cpu = -1;
        int rcvbuf_mb = 64;
    };

    void worker_loop(std::size_t index);
    int open_socket(const Channel& channel) const;
    void close_sockets();
    bool should_forward(const char* instrument) const;
    void update_filter(const std::vector<std::string>& instruments);
    static std::string normalize_symbol(const std::string& value);
    static void set_cpu_affinity(int cpu);

    std::vector<Channel> channels_;
    std::vector<int> sockets_;
    std::vector<std::thread> workers_;
    std::unordered_set<std::string> symbols_;
    bool filter_enabled_ = false;
    bool subscribe_all_ = true;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> snapshots_{0};
    std::atomic<std::uint64_t> filtered_{0};
    std::atomic<std::uint64_t> malformed_{0};
    std::atomic<std::uint64_t> ignored_{0};
    bool connected_ = false;
    bool logged_in_ = false;
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex subscription_mutex_;
};

WC_NAMESPACE_END

#endif
