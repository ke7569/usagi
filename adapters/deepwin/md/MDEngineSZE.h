#ifndef MDEngineSZE_H
#define MDEngineSZE_H

#include "IMDEngine.h"
#include "sze/market_data/SZEProtocol.h"
#include "sze/market_data/SZERecoverable.h"
#include "sze/market_data/SZEHealthState.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

WC_NAMESPACE_START

class MDEngineSZE : public IMDEngine
{
public:
    MDEngineSZE();
    ~MDEngineSZE() override;

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
    std::string name() const override { return "MDEngineSZE"; }

private:
    struct ChannelConfig
    {
        std::string multicast_ip;
        int port = 0;
        std::string iface_ip;
        std::string ifname;
        std::string bind_ip = "0.0.0.0";
        int bind_port = 0;
        int cpu = -1;
        int rcvbuf_mb = 64;
        int busy_poll_us = 0;
        int realtime_prio = 0;
    };

    struct ChannelStats
    {
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;
        std::uint64_t orders = 0;
        std::uint64_t executions = 0;
        std::uint64_t heartbeats = 0;
        std::uint64_t known_non_target = 0;
        std::uint64_t unknown_controls = 0;
        std::uint64_t filtered = 0;
        std::uint64_t malformed = 0;
        std::uint64_t unknown = 0;
        std::uint64_t recv_errors = 0;
    };

    struct RecoveryConfig
    {
        bool enabled = false;
        std::string backend = "socket";
        std::uint32_t trading_day = 0;
        std::string journal_directory;
        std::string journal_prefix = "sze";
        std::uint64_t journal_segment_bytes = 1ULL << 30U;
        std::uint32_t journal_max_payload_bytes = 256;
        std::uint64_t journal_min_free_bytes_after_allocate = 80ULL << 30U;
        int flush_interval_ms = 100;
        int flush_cpu = -1;
        std::string shm_path;
        std::uint32_t shm_capacity = 262144;
        std::uint32_t shm_max_payload_bytes = 256;
        bool replace_stale_shm = true;
        bool unlink_shm_on_clean_shutdown = true;
        std::string malformed_diagnostic_path;
        std::uint32_t malformed_diagnostic_max_records = 1000;
        bool health_state_enabled = true;
        std::string health_state_path;
    };

    struct RecoveryStats
    {
        RecoveryStats()
            : continuity_records(0), duplicates(0), missing_records(0),
              regressions(0), malformed_records(0), journal_events(0), journal_errors(0),
              ring_events(0), ring_errors(0), flush_errors(0),
              invalid_transitions(0), control_records(0),
              unknown_control_records(0), malformed_diagnostic_records(0),
              malformed_diagnostic_dropped(0)
        {
        }

        std::atomic<std::uint64_t> continuity_records;
        std::atomic<std::uint64_t> duplicates;
        std::atomic<std::uint64_t> missing_records;
        std::atomic<std::uint64_t> regressions;
        std::atomic<std::uint64_t> malformed_records;
        std::atomic<std::uint64_t> journal_events;
        std::atomic<std::uint64_t> journal_errors;
        std::atomic<std::uint64_t> ring_events;
        std::atomic<std::uint64_t> ring_errors;
        std::atomic<std::uint64_t> flush_errors;
        std::atomic<std::uint64_t> invalid_transitions;
        std::atomic<std::uint64_t> control_records;
        std::atomic<std::uint64_t> unknown_control_records;
        std::atomic<std::uint64_t> malformed_diagnostic_records;
        std::atomic<std::uint64_t> malformed_diagnostic_dropped;
    };

    struct FilterState
    {
        bool enabled = false;
        bool all = true;
        std::unordered_set<std::string> symbols;
    };

    struct DecodedEvent
    {
        DecodedEvent() : type(0), symbol_id(1000000U), raw_record(0), raw_length(0)
        {
            std::memset(&order, 0, sizeof(order));
            std::memset(&trade, 0, sizeof(trade));
            std::memset(&canonical, 0, sizeof(canonical));
        }

        std::uint8_t type;
        std::uint32_t symbol_id;
        LFL2OrderField order;
        LFL2TradeField trade;
        sze_recovery::CanonicalEvent canonical;
        const unsigned char* raw_record;
        std::size_t raw_length;
    };

private:

    void worker_loop(std::size_t index);
    int open_socket(const ChannelConfig& config) const;
    void close_sockets();
    void update_filter(const std::vector<std::string>& instruments);
    bool should_forward(const char* symbol) const;
    void log_stats(bool final);
    void load_recovery_config(const json& j_config);
    bool initialize_recovery();
    void shutdown_recovery(bool clean_shutdown);
    void recovery_flush_loop();
    bool observe_recovery_sequence(std::size_t index,
                                   std::uint32_t raw_sequence,
                                   std::uint16_t channel_number,
                                   std::uint64_t channel_sequence,
                                   std::uint64_t receive_mono_ns,
                                   std::uint64_t* expanded_sequence,
                                   bool* duplicate);
    void mark_recovery_invalid(sze_recovery::InvalidReason reason,
                               std::uint64_t feed_sequence);
    void mark_journal_degraded(const char* operation, int status);
    bool publish_recovery_event(DecodedEvent* event);
    void publish_recovery_metrics();
    void reset_recovery_resources(bool clean_shutdown, bool remove_ipc);
    bool initialize_malformed_diagnostics();
    void close_malformed_diagnostics();
    void flush_malformed_diagnostics();
    void record_wire_diagnostic(std::size_t channel_index,
                                std::uint64_t packet_number,
                                std::uint64_t receive_mono_ns,
                                std::size_t record_offset,
                                const unsigned char* record,
                                std::size_t record_length,
                                std::size_t datagram_length,
                                sze_md::DecodeStatus status,
                                sze_md::DecodeFailureReason reason,
                                bool invalidating);
    static bool replace_stale_ring(const std::string& path);
    static bool unlink_owned_generation(const std::string& path,
                                        std::uint64_t generation);
    static std::string normalize_symbol(const std::string& value);
    static std::string normalize_symbol(const char* value);
    static void copy_channel_json(const json& source, ChannelConfig* destination);
    static void set_cpu_affinity(int cpu);
    static void set_realtime_priority(int priority);

    std::vector<ChannelConfig> channels_;
    std::vector<int> sockets_;
    std::vector<ChannelStats> stats_;
    std::vector<std::thread> workers_;
    RecoveryConfig recovery_config_;
    RecoveryStats recovery_stats_;
    std::unique_ptr<sze_recovery::JournalWriter> recovery_journal_;
    std::unique_ptr<sze_recovery::ShmEventRing> recovery_ring_;
    std::unique_ptr<sze_health::CaptureHealthWriter> recovery_health_;
    std::vector<sze_recovery::FeedSequenceTracker> continuity_trackers_;
    std::thread recovery_flush_worker_;
    int malformed_diagnostic_fd_ = -1;
    void* malformed_diagnostic_mapping_ = nullptr;
    std::size_t malformed_diagnostic_mapping_size_ = 0;
    sze_md::DiagnosticFileHeader* malformed_diagnostic_header_ = nullptr;
    sze_md::DiagnosticRecord* malformed_diagnostic_records_ = nullptr;
    std::atomic<std::uint64_t> malformed_diagnostic_next_record_{0};
    std::atomic<bool> malformed_diagnostic_dirty_{false};
    std::atomic<bool> recovery_flush_running_{false};
    std::atomic<bool> recovery_journal_degraded_{false};
    std::atomic<std::uint64_t> recovery_ring_last_event_id_{0};
    std::atomic<bool> recovery_forced_invalid_{false};
    std::atomic<int> recovery_invalid_reason_{
        static_cast<int>(sze_recovery::kInvalidNone)};
    std::atomic<std::uint64_t> recovery_latest_feed_sequence_{0};
    std::atomic<bool> running_{false};
    bool connected_ = false;
    bool logged_in_ = false;
    int batch_size_ = 32;
    bool use_subscribe_filter_ = false;
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex stats_mutex_;
    mutable std::mutex subscription_mutex_;
    std::atomic<const FilterState*> filter_state_{nullptr};
    std::vector<std::unique_ptr<FilterState> > filter_versions_;
};

DECLARE_PTR(MDEngineSZE);

WC_NAMESPACE_END

#endif  // MDEngineSZE_H
