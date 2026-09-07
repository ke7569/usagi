#include "adapters/deepwin/md/MDEngineSZE.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class CapturingControlCenter : public kungfu::yijinjing::IControlCenter
{
public:
    void on_market_data(const LFMarketDataField*, short) override {}

    void on_market_data(const LFL2TradeField* data, short source) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sources_.push_back(source);
        kinds_.push_back('T');
        trades_.push_back(*data);
        condition_.notify_all();
    }

    void on_market_data(const LFL2OrderField* data, short source) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sources_.push_back(source);
        kinds_.push_back('O');
        orders_.push_back(*data);
        condition_.notify_all();
    }

    void on_market_data(const LFL2MarketDataField*, short) override {}
    void on_market_data(const LFL2IndexField*, short) override {}

    int insert_order(LFInputOrderField*, short, int, const std::string&) override { return 0; }
    int cancel_order(LFOrderActionField*, short, int, const std::string&) override { return 0; }
    int req_position(LFQryPositionField*, short, int, const std::string&) override { return 0; }
    int req_qry_limit_price(LFQryLimitPrice*, short, int, const std::string&) override { return 0; }
    int req_account(LFQryAccountField*, short, int, const std::string&) override { return 0; }
    bool subscribe_market_data(short, std::vector<std::string>&,
                               std::vector<std::string>&) override { return true; }
    void on_rtn_order(const LFRtnOrderField*, int, short, long) override {}
    void on_rtn_trade(const LFRtnTradeField*, int, short, long) override {}
    void on_rsp_order(const LFInputOrderField*, int, short, long, int,
                      const char*) override {}
    void on_rsp_exchange_state(const LFRspExchangeStateField*, short, long) override {}
    void on_rsp_order_action(const LFOrderActionField*, int, short, long, int,
                             const char*) override {}
    void on_rsp_limit_price(const LFMarketDataField*, int, short, long) override {}
    void on_rtn_trade_all(const LFRtnTradeField*, int, short, long) override {}
    void on_rsp_account(const LFRspAccountField*, int, short, long, int,
                        const char*) override {}
    void on_rtn_pos_option(const LFRspPositionField*, bool, int, short, long) override {}
    bool add_md(short, std::string) override { return true; }
    bool add_td(short, std::string) override { return true; }
    IntPair get_rid_pair(std::string) override
    {
        return IntPair();
    }
    bool set_str(void*) override { return true; }

    bool wait_for_events(std::size_t count, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                   [this, count]() { return kinds_.size() >= count; });
    }

    std::size_t event_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return kinds_.size();
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<short> sources_;
    std::vector<char> kinds_;
    std::vector<LFL2OrderField> orders_;
    std::vector<LFL2TradeField> trades_;
};

void set_symbol(std::uint8_t* destination, const char* value)
{
    std::memset(destination, 0, 9);
    std::memcpy(destination, value, std::strlen(value));
}

sze_md::SzeHpfHead make_head(std::uint8_t type, std::uint64_t sequence)
{
    sze_md::SzeHpfHead head{};
    head.sequence = static_cast<std::uint32_t>(sequence + 1000);
    head.message_type = type;
    head.security_type = 1;
    set_symbol(head.symbol, "000001");
    head.exchange_id = 101;
    head.quote_update_time = 20260720143015123ULL;
    head.channel_num = 2011;
    head.sequence_num = sequence;
    head.md_stream_id = 1;
    return head;
}

void send_packet(const void* data, std::size_t length, const char* group, int port)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    in_addr loopback;
    assert(::inet_pton(AF_INET, "127.0.0.1", &loopback) == 1);
    assert(::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                        &loopback, sizeof(loopback)) == 0);
    unsigned char enabled = 1;
    assert(::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                        &enabled, sizeof(enabled)) == 0);

    sockaddr_in destination;
    std::memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<std::uint16_t>(port));
    assert(::inet_pton(AF_INET, group, &destination.sin_addr) == 1);
    assert(::sendto(fd, data, length, 0,
                    reinterpret_cast<const sockaddr*>(&destination),
                    sizeof(destination)) == static_cast<ssize_t>(length));
    ::close(fd);
}

}  // namespace

int main()
{
    const char* group = "239.255.0.89";
    const int port = 39089;

    {
        kungfu::wingchun::MDEngineSZE invalid;
        bool rejected = false;
        try {
            invalid.load(json::object());
        } catch (const std::exception&) {
            rejected = true;
        }
        assert(rejected);
    }

    CapturingControlCenter control;
    kungfu::wingchun::MDEngineSZE engine;
    engine.set_cc(&control);
    const json config = {
        {"batch", 8},
        {"use_subscribe_filter", true},
        {"subscribe_all", false},
        {"channels", {{
            {"multicast_ip", group},
            {"port", port},
            {"iface_ip", "127.0.0.1"},
            {"ifname", "lo"},
            {"bind_ip", "0.0.0.0"},
            {"bind_port", port},
            {"rcvbuf_mb", 4}
        }}}
    };
    engine.load(config);
    engine.subscribeOrderTrade(std::vector<std::string>(1, "000001.SZ"),
                               std::vector<std::string>());
    assert(engine.start());
    assert(engine.is_logged_in());

    struct Packet {
        sze_md::SzeHpfOrder order;
        sze_md::SzeHpfExecution execution;
    } __attribute__((packed));
    Packet packet{};
    packet.order.head = make_head(sze_md::kOrderMessage, 1001);
    packet.order.order_price = 123450;
    packet.order.order_quantity = 1000;
    packet.order.side_flag = '1';
    packet.order.order_type = '2';
    packet.execution.head = make_head(sze_md::kExecutionMessage, 1002);
    packet.execution.trade_buy_num = 1001;
    packet.execution.trade_sell_num = 1003;
    packet.execution.trade_price = 123450;
    packet.execution.trade_quantity = 500;
    packet.execution.trade_type = 'F';
    send_packet(&packet, sizeof(packet), group, port);

    assert(control.wait_for_events(2, 2000));
    {
        std::lock_guard<std::mutex> lock(control.mutex_);
        assert(control.kinds_.size() == 2);
        assert(control.kinds_[0] == 'O');
        assert(control.kinds_[1] == 'T');
        assert(control.sources_[0] == 88 && control.sources_[1] == 88);
        assert(control.orders_[0].ApplSeqNum == 1001);
        assert(control.trades_[0].BidApplSeqNum == 1001);
        assert(control.trades_[0].OfferApplSeqNum == 1003);
    }

    Packet filtered_packet = packet;
    set_symbol(filtered_packet.order.head.symbol, "000002");
    set_symbol(filtered_packet.execution.head.symbol, "000002");
    filtered_packet.order.head.sequence_num = 2001;
    filtered_packet.execution.head.sequence_num = 2002;
    send_packet(&filtered_packet, sizeof(filtered_packet), group, port);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(control.event_count() == 2);

    engine.subscribeOrderTrade(std::vector<std::string>(1, "000002"),
                               std::vector<std::string>());
    send_packet(&filtered_packet, sizeof(filtered_packet), group, port);
    assert(control.wait_for_events(4, 2000));
    {
        std::lock_guard<std::mutex> lock(control.mutex_);
        assert(std::strcmp(control.orders_[1].InstrumentID, "000002") == 0);
        assert(std::strcmp(control.trades_[1].InstrumentID, "000002") == 0);
    }

    // Subscription callbacks are additive: adding 000002 must not remove the
    // previously requested 000001 stream.
    send_packet(&packet, sizeof(packet), group, port);
    assert(control.wait_for_events(6, 2000));
    {
        std::lock_guard<std::mutex> lock(control.mutex_);
        assert(std::strcmp(control.orders_[2].InstrumentID, "000001") == 0);
        assert(std::strcmp(control.trades_[2].InstrumentID, "000001") == 0);
    }

    unsigned char malformed[73] = {};
    std::memcpy(malformed, &packet.order, sizeof(packet.order));
    send_packet(malformed, sizeof(malformed), group, port);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(control.event_count() == 6);

    struct MixedInvalidPacket {
        sze_md::SzeHpfOrder first;
        sze_md::SzeHpfOrder second;
    } __attribute__((packed));
    MixedInvalidPacket mixed{};
    mixed.first = packet.order;
    mixed.second = packet.order;
    mixed.second.head.message_type = 99;
    mixed.second.head.sequence_num = 9001;
    send_packet(&mixed, sizeof(mixed), group, port);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(control.event_count() == 6);

    sze_md::SzeHpfHeartbeat heartbeat{};
    heartbeat.message_type = sze_md::kTickHeartbeatMessage;
    send_packet(&heartbeat, sizeof(heartbeat), group, port);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(control.event_count() == 6);

    assert(engine.stop());
    engine.logout();
    assert(!engine.is_logged_in());

    char recovery_directory_template[] = "/tmp/sze_md_loopback_recovery_XXXXXX";
    char* recovery_directory_value = ::mkdtemp(recovery_directory_template);
    assert(recovery_directory_value != 0);
    const std::string recovery_directory(recovery_directory_value);
    const std::string recovery_shm = recovery_directory + "/events.shm";
    const int recovery_port = port + 1;
    const json recovery_config = {
        {"batch", 8},
        {"use_subscribe_filter", true},
        {"subscribe_all", false},
        {"channels", {{
            {"multicast_ip", group},
            {"port", recovery_port},
            {"iface_ip", "127.0.0.1"},
            {"ifname", "lo"},
            {"bind_ip", "0.0.0.0"},
            {"bind_port", recovery_port},
            {"rcvbuf_mb", 4}
        }}},
        {"recoverable_pipeline", {
            {"enabled", true},
            {"backend", "socket"},
            {"trading_day", 20260720},
            {"journal_directory", recovery_directory},
            {"journal_prefix", "recoverable"},
            {"journal_segment_bytes", 8192},
            {"journal_max_payload_bytes", 128},
            {"flush_interval_ms", 10},
            {"shm_path", recovery_shm},
            {"shm_capacity", 8},
            {"shm_max_payload_bytes", 128},
            {"replace_stale_shm", true},
            {"unlink_shm_on_clean_shutdown", true},
            {"malformed_diagnostic_max_records", 16}
        }}
    };

    CapturingControlCenter recovery_control;
    kungfu::wingchun::MDEngineSZE recovery_engine;
    recovery_engine.set_cc(&recovery_control);
    recovery_engine.load(recovery_config);
    recovery_engine.subscribeOrderTrade(std::vector<std::string>(1, "000001.SZ"),
                                        std::vector<std::string>());
    assert(recovery_engine.start());

    Packet first_selected{};
    first_selected.order.head = make_head(sze_md::kOrderMessage, 1001);
    first_selected.order.order_price = 123450;
    first_selected.order.order_quantity = 1000;
    first_selected.order.side_flag = '1';
    first_selected.order.order_type = '2';
    first_selected.execution.head = make_head(sze_md::kExecutionMessage, 1002);
    first_selected.execution.trade_buy_num = 1001;
    first_selected.execution.trade_sell_num = 1003;
    first_selected.execution.trade_price = 123450;
    first_selected.execution.trade_quantity = 500;
    first_selected.execution.trade_type = 'F';
    send_packet(&first_selected, sizeof(first_selected), group, recovery_port);
    assert(recovery_control.wait_for_events(2, 2000));

    Packet unselected = first_selected;
    unselected.order.head = make_head(sze_md::kOrderMessage, 1003);
    unselected.execution.head = make_head(sze_md::kExecutionMessage, 1004);
    set_symbol(unselected.order.head.symbol, "000002");
    set_symbol(unselected.execution.head.symbol, "000002");
    send_packet(&unselected, sizeof(unselected), group, recovery_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(recovery_control.event_count() == 2);

    Packet second_selected = first_selected;
    second_selected.order.head = make_head(sze_md::kOrderMessage, 1005);
    second_selected.execution.head = make_head(sze_md::kExecutionMessage, 1006);
    send_packet(&second_selected, sizeof(second_selected), group, recovery_port);
    assert(recovery_control.wait_for_events(4, 2000));

    sze_recovery::ShmEventRing ring_reader;
    assert(ring_reader.attach(recovery_shm));
    assert(ring_reader.latest_event_id() == 4U);
    assert(ring_reader.continuity_state() == sze_recovery::kContinuityValid);
    for (std::uint64_t event_id = 1U; event_id <= 4U; ++event_id) {
        sze_recovery::CanonicalEvent event;
        unsigned char raw[128];
        assert(ring_reader.read(event_id, &event, raw, sizeof(raw)) ==
               sze_recovery::kRingReadOk);
        assert(event.event_id == event_id);
        assert(event.payload_size == sizeof(sze_md::SzeHpfOrder));
        const sze_md::SzeHpfHead* head =
            reinterpret_cast<const sze_md::SzeHpfHead*>(raw);
        assert(std::memcmp(head->symbol, "000001", 6) == 0);
    }

    // Official control and non-equity tick records advance the source
    // sequence but do not enter the equity callback or journal.
    sze_md::SzeHpfHeartbeat snapshot_heartbeat{};
    snapshot_heartbeat.sequence = 2007U;
    snapshot_heartbeat.message_type = sze_md::kSnapshotHeartbeatMessage;
    send_packet(&snapshot_heartbeat, sizeof(snapshot_heartbeat), group,
                recovery_port);
    unsigned char index_record[sze_md::kIndexRecordSize] = {};
    const std::uint32_t index_feed_sequence = 2008U;
    std::memcpy(index_record, &index_feed_sequence,
                sizeof(index_feed_sequence));
    index_record[8] = sze_md::kIndexMessage;
    send_packet(index_record, sizeof(index_record), group, recovery_port);
    sze_md::SzeHpfOrder bond_order = first_selected.order;
    bond_order.head = make_head(sze_md::kBondOrderMessage, 7001);
    bond_order.head.sequence = 2009U;
    send_packet(&bond_order, sizeof(bond_order), group, recovery_port);
    sze_md::SzeHpfHeartbeat unknown_control{};
    unknown_control.sequence = 2010U;
    unknown_control.message_type = 199U;
    send_packet(&unknown_control, sizeof(unknown_control), group, recovery_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(recovery_control.event_count() == 4);
    assert(ring_reader.latest_event_id() == 4U);
    assert(ring_reader.continuity_state() == sze_recovery::kContinuityValid);

    sze_recovery::JournalConfig journal_config;
    journal_config.directory = recovery_directory;
    journal_config.prefix = "recoverable";
    journal_config.trading_day = 20260720U;
    journal_config.source_id = 88U;
    journal_config.segment_bytes = 8192U;
    journal_config.max_payload_bytes = 128U;
    sze_recovery::JournalReader journal_reader;
    assert(journal_reader.open(journal_config).status == sze_recovery::kJournalOk);
    for (std::uint64_t event_id = 1U; event_id <= 4U; ++event_id) {
        sze_recovery::CanonicalEvent event;
        unsigned char raw[128];
        assert(journal_reader.next(&event, raw, sizeof(raw)) ==
               sze_recovery::kJournalOk);
        assert(event.event_id == event_id);
    }
    sze_recovery::CanonicalEvent live_tail;
    unsigned char live_tail_raw[128];
    assert(journal_reader.next(&live_tail, live_tail_raw, sizeof(live_tail_raw)) ==
           sze_recovery::kJournalWouldBlock);
    journal_reader.close();

    // A valid-length but unsupported message must remain fail-closed and
    // leave enough raw evidence to identify the protocol variant.
    unsigned char malformed_recovery[sze_md::kIndexRecordSize - 1U] = {};
    malformed_recovery[8] = sze_md::kIndexMessage;
    send_packet(malformed_recovery, sizeof(malformed_recovery), group, recovery_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(ring_reader.continuity_state() == sze_recovery::kContinuityInvalid);

    sze_md::SzeHpfOrder unsupported = first_selected.order;
    unsupported.head = make_head(99, 1011);
    send_packet(&unsupported, sizeof(unsupported), group, recovery_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(ring_reader.continuity_state() == sze_recovery::kContinuityInvalid);

    struct MixedRecoveryPacket {
        sze_md::SzeHpfOrder first;
        sze_md::SzeHpfOrder unsupported;
        sze_md::SzeHpfExecution last;
    } __attribute__((packed));
    MixedRecoveryPacket mixed_recovery{};
    mixed_recovery.first = first_selected.order;
    mixed_recovery.first.head = make_head(sze_md::kOrderMessage, 1012);
    mixed_recovery.unsupported = first_selected.order;
    mixed_recovery.unsupported.head = make_head(99, 1013);
    mixed_recovery.last = first_selected.execution;
    mixed_recovery.last.head = make_head(sze_md::kExecutionMessage, 1014);
    send_packet(&mixed_recovery, sizeof(mixed_recovery), group, recovery_port);
    assert(recovery_control.wait_for_events(6, 2000));
    assert(ring_reader.latest_event_id() == 6U);

    Packet gap_selected = first_selected;
    gap_selected.order.head = make_head(sze_md::kOrderMessage, 1015);
    gap_selected.execution.head = make_head(sze_md::kExecutionMessage, 1016);
    send_packet(&gap_selected, sizeof(gap_selected), group, recovery_port);
    assert(recovery_control.wait_for_events(8, 2000));
    assert(ring_reader.latest_event_id() == 8U);
    assert(ring_reader.continuity_state() == sze_recovery::kContinuityInvalid);
    assert(ring_reader.readiness_state() == sze_recovery::kReadinessNotReady);
    ring_reader.close();

    assert(recovery_engine.stop());
    recovery_engine.logout();
    assert(::access(recovery_shm.c_str(), F_OK) != 0);

    sze_recovery::JournalReader closed_reader;
    const sze_recovery::JournalOpenResult closed_open =
        closed_reader.open(journal_config);
    assert(closed_open.status == sze_recovery::kJournalOk);
    assert(!closed_open.unclean_restart);
    assert(closed_open.last_event_id == 8U);
    assert(closed_open.continuity_state == sze_recovery::kContinuityInvalid);
    closed_reader.close();

    CapturingControlCenter restarted_control;
    kungfu::wingchun::MDEngineSZE restarted_engine;
    restarted_engine.set_cc(&restarted_control);
    restarted_engine.load(recovery_config);
    restarted_engine.subscribeOrderTrade(
        std::vector<std::string>(1, "000001.SZ"), std::vector<std::string>());
    assert(restarted_engine.start());
    sze_recovery::ShmEventRing restarted_ring;
    assert(restarted_ring.attach(recovery_shm));
    assert(restarted_ring.continuity_state() == sze_recovery::kContinuityInvalid);
    assert(restarted_ring.readiness_state() == sze_recovery::kReadinessNotReady);
    restarted_ring.close();
    assert(restarted_engine.stop());
    restarted_engine.logout();

    const std::string diagnostic_path = recovery_directory +
        "/recoverable_20260720_malformed.bin";
    std::ifstream diagnostic(diagnostic_path.c_str(), std::ios::binary);
    assert(diagnostic.is_open());
    sze_md::DiagnosticFileHeader diagnostic_header{};
    diagnostic.read(reinterpret_cast<char*>(&diagnostic_header),
                    sizeof(diagnostic_header));
    assert(diagnostic.gcount() == sizeof(diagnostic_header));
    assert(diagnostic_header.magic == sze_md::kDiagnosticFileMagic);
    assert(diagnostic_header.version == sze_md::kDiagnosticFileVersion);
    assert(diagnostic_header.record_size == sizeof(sze_md::DiagnosticRecord));
    assert(diagnostic_header.capacity == 16U);
    assert(diagnostic_header.committed_records >= 3U);
    assert(std::strstr(diagnostic_header.build_id,
                       "sze-md-20260724-protocol-v2") != 0);
    bool found_invalid_length = false;
    bool found_unsupported = false;
    bool found_unknown_control = false;
    for (std::uint64_t index = 0;
         index < diagnostic_header.committed_records; ++index) {
        sze_md::DiagnosticRecord record{};
        diagnostic.read(reinterpret_cast<char*>(&record), sizeof(record));
        assert(diagnostic.gcount() == sizeof(record));
        assert(record.commit == sze_md::kDiagnosticRecordCommit);
        if (record.failure_reason == static_cast<std::uint8_t>(
                sze_md::DecodeFailureReason::kInvalidLength)) {
            found_invalid_length = true;
        }
        if (record.message_type == 99U &&
            record.failure_reason == static_cast<std::uint8_t>(
                sze_md::DecodeFailureReason::kUnsupportedMessageType)) {
            found_unsupported = true;
        }
        if (record.message_type == 199U &&
            (record.flags & sze_md::kDiagnosticNonInvalidating) != 0U) {
            found_unknown_control = true;
        }
    }
    assert(found_invalid_length);
    assert(found_unsupported);
    assert(found_unknown_control);
    diagnostic.close();

    const std::string journal_path = recovery_directory +
        "/recoverable_20260720_s88_000000.szej";
    assert(::unlink(diagnostic_path.c_str()) == 0);
    assert(::unlink(journal_path.c_str()) == 0);
    assert(::rmdir(recovery_directory.c_str()) == 0);
    return 0;
}
