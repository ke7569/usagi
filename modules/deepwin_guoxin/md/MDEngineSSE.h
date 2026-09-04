#ifndef MDEngineSSE_H
#define MDEngineSSE_H

#include "IMDEngine.h"
#include "../../../sse-t0/market_data/sse_primary_decoder.h"
#include "common/UdpChannelRuntime.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>

WC_NAMESPACE_START

class MDEngineSSE : public IMDEngine {
public:
    MDEngineSSE();
    ~MDEngineSSE() override;
    void init() override;
    void load(const json& config) override;
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
    std::string name() const override { return "MDEngineSSE"; }
private:
    void run();
    void on_datagram(const deepwin_market_data::Datagram& datagram);
    void on_tick(const sse_live::TickEvent& tick, std::uint64_t realtime_ns);
    bool seen_tick(const sse_live::TickEvent& tick);
    bool seen_snapshot(const sse_live::Snapshot& snapshot);
    bool allowed(const std::string& symbol) const;
    std::vector<deepwin_market_data::ChannelSpec> channels_;
    std::vector<std::string> symbols_;
    bool filter_enabled_;
    std::atomic<bool> connected_;
    std::atomic<bool> logged_in_;
    std::atomic<bool> running_;
    std::thread worker_;
    std::string error_;
    std::mutex mutex_;
    std::unordered_set<std::string> seen_keys_;
    deepwin_market_data::UdpChannelRuntime runtime_;
};

WC_NAMESPACE_END

#endif
