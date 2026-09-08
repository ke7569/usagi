#include "sze/runtime/sze_stream_processor.h"

#include "common/stream/UdpChannelRuntime.h"
#include "sze/market_data/SZEProtocol.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace {

using deepwin_market_data::StreamEvent;
using deepwin_market_data::kDatagramEvent;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        char name[] = "/tmp/sze-processor-test-XXXXXX";
        char* result = ::mkdtemp(name);
        assert(result != 0);
        path = result;
    }
    ~TemporaryDirectory() {
        remove_tree(path);
        ::rmdir(path.c_str());
    }

    std::string path;

private:
    static void remove_tree(const std::string& directory) {
        DIR* handle = ::opendir(directory.c_str());
        if (handle == 0) return;
        for (;;) {
            dirent* entry = ::readdir(handle);
            if (entry == 0) break;
            if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
            const std::string child = directory + "/" + entry->d_name;
            struct stat info;
            if (::lstat(child.c_str(), &info) != 0) continue;
            if (S_ISDIR(info.st_mode)) {
                remove_tree(child);
                ::rmdir(child.c_str());
            } else {
                ::unlink(child.c_str());
            }
        }
        ::closedir(handle);
    }
};

deepwin_market_data::ChannelSpec loopback_channel() {
    const int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(socket >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    ::close(socket);
    deepwin_market_data::ChannelSpec result;
    result.name = "sze-loopback";
    result.group = "127.0.0.1";
    result.interface_ip = "127.0.0.1";
    result.port = ntohs(address.sin_port);
    return result;
}

void send_packet(int socket,
                 const deepwin_market_data::ChannelSpec& channel,
                 const std::vector<unsigned char>& bytes) {
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(channel.port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::sendto(socket, bytes.data(), bytes.size(), 0,
                    reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
           static_cast<ssize_t>(bytes.size()));
}

struct SavedStreamEvent {
    StreamEvent event;
    std::vector<unsigned char> bytes;

    explicit SavedStreamEvent(const StreamEvent& source) : event(source), bytes() {
        if (source.size != 0U) {
            bytes.assign(source.data, source.data + source.size);
        }
        event.data = 0;
    }

    bool operator==(const SavedStreamEvent& other) const {
        return std::tie(event.kind, event.sequence, event.monotonic_ns,
                        event.realtime_ns, event.receive_batch, event.channel_id,
                        event.batch_index, event.batch_size, event.source_ipv4,
                        event.source_port, event.timestamp_flags, event.size, bytes) ==
               std::tie(other.event.kind, other.event.sequence, other.event.monotonic_ns,
                        other.event.realtime_ns, other.event.receive_batch,
                        other.event.channel_id, other.event.batch_index,
                        other.event.batch_size, other.event.source_ipv4,
                        other.event.source_port, other.event.timestamp_flags,
                        other.event.size, other.bytes);
    }
};

void set_symbol(std::uint8_t* destination, const char* value) {
    std::memset(destination, 0, 9U);
    std::memcpy(destination, value, std::strlen(value));
}

sze_md::SzeHpfHead make_head(std::uint8_t type,
                             std::uint32_t feed_sequence,
                             std::uint64_t application_sequence,
                             const char* symbol,
                             std::uint64_t exchange_time) {
    sze_md::SzeHpfHead head = {};
    head.sequence = feed_sequence;
    head.message_type = type;
    head.security_type = 1;
    set_symbol(head.symbol, symbol);
    head.exchange_id = 101;
    head.quote_update_time = exchange_time;
    head.channel_num = 7;
    head.sequence_num = application_sequence;
    head.md_stream_id = 1;
    return head;
}

mix153060::StaticInputs inputs(const char* instrument) {
    mix153060::StaticInputs value;
    value.instrument = instrument;
    value.trading_date = 20260720;
    value.average_amount = 1000000.0;
    value.turnover_threshold = 1000000000.0;
    value.free_share = 1000000.0;
    value.pre_close = 10.0;
    value.upper_limit = 11.0;
    value.lower_limit = 9.0;
    value.history_volatility_20d = 0.2;
    return value;
}

struct EventBytes {
    std::vector<unsigned char> bytes;
    StreamEvent event;

    EventBytes(const std::vector<unsigned char>& value,
               std::uint64_t ingress_sequence,
               std::uint64_t receive_ns)
        : bytes(value), event() {
        event.kind = kDatagramEvent;
        event.sequence = ingress_sequence;
        event.realtime_ns = receive_ns;
        event.monotonic_ns = receive_ns;
        event.channel_id = 0;
        event.data = bytes.data();
        event.size = bytes.size();
    }
};

std::vector<unsigned char> order(std::uint32_t feed,
                                 std::uint64_t application,
                                 const char* symbol,
                                 bool buy,
                                 std::uint64_t time) {
    sze_md::SzeHpfOrder value = {};
    value.head = make_head(sze_md::kOrderMessage, feed, application, symbol, time);
    value.order_price = 100000;
    value.order_quantity = 10000;
    value.side_flag = buy ? '1' : '2';
    value.order_type = '2';
    return std::vector<unsigned char>(
        reinterpret_cast<unsigned char*>(&value),
        reinterpret_cast<unsigned char*>(&value) + sizeof(value));
}

std::vector<unsigned char> execution(std::uint32_t feed,
                                     std::uint64_t application,
                                     const char* symbol,
                                     std::uint64_t buy_id,
                                     std::uint64_t sell_id,
                                     std::uint64_t time) {
    sze_md::SzeHpfExecution value = {};
    value.head = make_head(sze_md::kExecutionMessage, feed, application, symbol, time);
    value.trade_buy_num = static_cast<std::int64_t>(buy_id);
    value.trade_sell_num = static_cast<std::int64_t>(sell_id);
    value.trade_price = 100000;
    value.trade_quantity = 10000;
    value.trade_type = 'F';
    return std::vector<unsigned char>(
        reinterpret_cast<unsigned char*>(&value),
        reinterpret_cast<unsigned char*>(&value) + sizeof(value));
}

std::uint64_t exchange_time(std::uint64_t seconds_after_open) {
    const std::uint64_t hour = 9;
    const std::uint64_t minute = 30;
    const std::uint64_t total_seconds = hour * 3600U + minute * 60U +
                                        seconds_after_open;
    const std::uint64_t h = total_seconds / 3600U;
    const std::uint64_t m = (total_seconds / 60U) % 60U;
    const std::uint64_t s = total_seconds % 60U;
    return 20260720ULL * 1000000000ULL + h * 10000000ULL +
           m * 100000ULL + s * 1000ULL;
}

std::vector<EventBytes> make_events() {
    std::vector<EventBytes> result;
    result.reserve(1000U);
    std::uint32_t feed = 1U;
    std::uint64_t app = 1U;
    for (std::size_t pair = 0; pair < 333U; ++pair) {
        const char* symbol = pair % 2U == 0U ? "000001" : "000002";
        const std::uint64_t time = exchange_time(static_cast<std::uint64_t>(pair) * 40U);
        const std::uint64_t buy_id = app;
        result.push_back(EventBytes(order(feed++, app++, symbol, true, time),
                                    feed - 1U, 1700000000000000000ULL + feed * 1000U));
        const std::uint64_t sell_id = app;
        result.push_back(EventBytes(order(feed++, app++, symbol, false, time + 1U),
                                    feed - 1U, 1700000000000000000ULL + feed * 1000U));
        result.push_back(EventBytes(execution(feed++, app++, symbol, buy_id, sell_id,
                                              exchange_time(static_cast<std::uint64_t>(pair) * 40U + 31U)),
                                    feed - 1U, 1700000000000000000ULL + feed * 1000U));
    }
    result.push_back(EventBytes(order(feed++, app++, "000001", true,
                                      exchange_time(333U * 40U)),
                                feed - 1U, 1700000000000000000ULL + feed * 1000U));
    assert(result.size() == 1000U);
    return result;
}

void compare_sample(const sze_stream::ProcessedSample& left,
                    const sze_stream::ProcessedSample& right) {
    assert(left.sample.instrument == right.sample.instrument);
    assert(left.sample.app_sequence == right.sample.app_sequence);
    assert(left.sample.exchange_time_us == right.sample.exchange_time_us);
    assert(left.sample.local_time_us == right.sample.local_time_us);
    assert(left.sample.cut_index == right.sample.cut_index);
    assert(left.ingress_sequence == right.ingress_sequence);
    assert(left.record_offset == right.record_offset);
    assert(left.record_size == right.record_size);
    assert(!left.prediction_valid && !right.prediction_valid);
    assert(std::isnan(left.prediction) && std::isnan(right.prediction));
    for (std::size_t i = 0; i < left.sample.factors.size(); ++i) {
        assert(left.sample.factors[i] == right.sample.factors[i]);
    }
    for (std::size_t i = 0; i < left.sample.bid_price.size(); ++i) {
        assert(left.sample.bid_price[i] == right.sample.bid_price[i]);
        assert(left.sample.ask_price[i] == right.sample.ask_price[i]);
        assert(left.sample.bid_volume[i] == right.sample.bid_volume[i]);
        assert(left.sample.ask_volume[i] == right.sample.ask_volume[i]);
    }
}

void test_live_replay_and_isolation() {
    const std::vector<EventBytes> events = make_events();
    std::vector<sze_stream::ProcessedSample> live_samples;
    std::vector<sze_stream::ProcessedSample> replay_samples;
    std::vector<mix153060::StaticInputs> configured;
    configured.push_back(inputs("000001.SZ"));
    configured.push_back(inputs("000002.SZ"));
    sze_stream::SzeStreamProcessor live(
        configured, 0, 1,
        [&](const sze_stream::ProcessedSample& sample) { live_samples.push_back(sample); });
    sze_stream::SzeStreamProcessor replay(
        configured, 0, 1,
        [&](const sze_stream::ProcessedSample& sample) { replay_samples.push_back(sample); });
    for (std::size_t i = 0; i < events.size(); ++i) {
        live.on_event(events[i].event);
        replay.on_event(events[i].event);
    }
    assert(live.available() && replay.available());
    assert(live.stats().records == 1000U);
    assert(live.stats().orders > 0U && live.stats().executions > 0U);
    assert(!live_samples.empty() && live_samples.size() == replay_samples.size());
    bool saw_first = false;
    bool saw_second = false;
    for (std::size_t i = 0; i < live_samples.size(); ++i) {
        compare_sample(live_samples[i], replay_samples[i]);
        bool nonzero_factor = false;
        for (std::size_t factor = 0; factor < live_samples[i].sample.factors.size(); ++factor) {
            nonzero_factor = nonzero_factor || live_samples[i].sample.factors[factor] != 0.0f;
        }
        assert(nonzero_factor);
        saw_first = saw_first || live_samples[i].sample.instrument == "000001.SZ";
        saw_second = saw_second || live_samples[i].sample.instrument == "000002.SZ";
    }
    assert(saw_first && saw_second);
}

void test_market_data_stream_capture_replay() {
    const std::vector<EventBytes> wire_events = make_events();
    const std::vector<mix153060::StaticInputs> configured = {
        inputs("000001.SZ"), inputs("000002.SZ")};
    TemporaryDirectory temporary;
    const deepwin_market_data::ChannelSpec channel = loopback_channel();
    deepwin_market_data::StreamOptions options;
    options.queue_capacity = 4096;
    options.max_datagram_bytes = 1024;
    options.receive_batch_size = 64;
    options.idle_gap_ns = 0;
    options.segment_bytes = 32768;
    options.flush_interval_ms = 5;
    options.recording_directory = temporary.path + "/capture";

    std::vector<SavedStreamEvent> live_events;
    std::vector<SavedStreamEvent> replay_events;
    std::vector<sze_stream::ProcessedSample> live_samples;
    std::vector<sze_stream::ProcessedSample> replay_samples;
    sze_stream::SzeStreamProcessor live_processor(
        configured, 0, 1,
        [&](const sze_stream::ProcessedSample& sample) { live_samples.push_back(sample); });
    deepwin_market_data::MarketDataStream live_stream;
    bool run_ok = false;
    std::atomic<bool> ready(false), done(false);
    std::string error;
    std::thread receiver([&]() {
        run_ok = live_stream.run(
            std::vector<deepwin_market_data::ChannelSpec>(1, channel), options,
            [&](const StreamEvent& event) {
                live_events.push_back(SavedStreamEvent(event));
                live_processor.on_event(event);
                ready.store(true);
            }, 2000, &error);
        done.store(true);
    });
    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(sender >= 0);
    sze_md::SzeHpfHeartbeat heartbeat = {};
    heartbeat.message_type = sze_md::kTickHeartbeatMessage;
    const std::vector<unsigned char> handshake(
        reinterpret_cast<const unsigned char*>(&heartbeat),
        reinterpret_cast<const unsigned char*>(&heartbeat) + sizeof(heartbeat));
    for (int i = 0; i < 500 && !ready.load() && !done.load(); ++i) {
        send_packet(sender, channel, handshake);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(ready.load());
    for (std::size_t i = 0; i < wire_events.size(); ++i) {
        send_packet(sender, channel, wire_events[i].bytes);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    ::close(sender);
    receiver.join();
    if (!run_ok) std::cerr << "SZE capture failed: " << error << '\n';
    assert(run_ok && live_processor.available());
    assert(live_events.size() == wire_events.size() + live_processor.stats().heartbeats);
    assert(live_stream.stats().received_datagrams == live_events.size());
    assert(!live_samples.empty());
    assert(live_stream.stats().clean_recording);

    sze_stream::SzeStreamProcessor replay_processor(
        configured, 0, 1,
        [&](const sze_stream::ProcessedSample& sample) { replay_samples.push_back(sample); });
    deepwin_market_data::MarketDataStream replay_stream;
    assert(replay_stream.replay(
        options.recording_directory,
        [&](const StreamEvent& event) {
            replay_events.push_back(SavedStreamEvent(event));
            replay_processor.on_event(event);
        }, &error));
    assert(replay_processor.available());
    assert(live_events == replay_events);
    assert(live_samples.size() == replay_samples.size());
    for (std::size_t i = 0; i < live_samples.size(); ++i) {
        compare_sample(live_samples[i], replay_samples[i]);
    }
}

void test_control_records_preserve_continuity() {
    const std::vector<mix153060::StaticInputs> configured(1, inputs("000001"));
    sze_md::SzeHpfHeartbeat heartbeat = {};
    heartbeat.sequence = 1U;
    heartbeat.message_type = sze_md::kTickHeartbeatMessage;
    std::vector<unsigned char> index(sze_md::kIndexRecordSize, 0U);
    const std::uint32_t index_sequence = 2U;
    std::memcpy(index.data(), &index_sequence, sizeof(index_sequence));
    index[8] = sze_md::kIndexMessage;
    std::vector<unsigned char> combined(
        reinterpret_cast<unsigned char*>(&heartbeat),
        reinterpret_cast<unsigned char*>(&heartbeat) + sizeof(heartbeat));
    combined.insert(combined.end(), index.begin(), index.end());
    const std::vector<unsigned char> next = order(3U, 1U, "000001", true,
                                                  exchange_time(0));
    combined.insert(combined.end(), next.begin(), next.end());
    EventBytes event(combined, 1U, 1700000000000000000ULL);
    sze_stream::SzeStreamProcessor processor(configured, 0, 1,
                                              sze_stream::SampleCallback());
    processor.on_event(event.event);
    assert(processor.available());
    assert(processor.stats().records == 3U);
    assert(processor.stats().heartbeats == 1U);
    assert(processor.stats().known_non_target == 1U);
    assert(processor.stats().orders == 1U);
}

void test_concatenated_record_provenance() {
    const std::vector<mix153060::StaticInputs> configured(1, inputs("000001"));
    std::vector<unsigned char> combined = order(1U, 1U, "000001", true,
                                                exchange_time(0));
    const std::vector<unsigned char> second = order(2U, 2U, "000001", false,
                                                    exchange_time(1));
    const std::vector<unsigned char> third = execution(
        3U, 3U, "000001", 1U, 2U, exchange_time(131U));
    combined.insert(combined.end(), second.begin(), second.end());
    combined.insert(combined.end(), third.begin(), third.end());
    std::vector<sze_stream::ProcessedSample> samples;
    sze_stream::SzeStreamProcessor processor(
        configured, 0, 1,
        [&](const sze_stream::ProcessedSample& sample) { samples.push_back(sample); });
    EventBytes event(combined, 42U, 1700000000000000000ULL);
    processor.on_event(event.event);
    assert(samples.size() == 1U);
    assert(samples[0].ingress_sequence == 42U);
    assert(samples[0].record_offset == 2U * sizeof(sze_md::SzeHpfOrder));
    assert(samples[0].record_size == sizeof(sze_md::SzeHpfExecution));
}

void send_one(sze_stream::SzeStreamProcessor* processor,
              const std::vector<unsigned char>& bytes,
              std::uint64_t ingress_sequence) {
    EventBytes event(bytes, ingress_sequence,
                     1700000000000000000ULL + ingress_sequence * 1000U);
    processor->on_event(event.event);
}

void test_first_post_open_event_is_boundary_only() {
    const std::vector<mix153060::StaticInputs> configured(1, inputs("000001"));
    std::vector<sze_stream::ProcessedSample> samples;
    sze_stream::SzeStreamProcessor processor(
        configured, 0, 1,
        [&](const sze_stream::ProcessedSample& sample) { samples.push_back(sample); });
    send_one(&processor, order(1U, 1U, "000001", true, exchange_time(0)), 1U);
    send_one(&processor, order(2U, 2U, "000001", false, exchange_time(0) + 1U), 2U);
    send_one(&processor, execution(3U, 3U, "000001", 1U, 2U,
                                  exchange_time(0) + 30U), 3U);
    assert(samples.empty());

    send_one(&processor, order(4U, 4U, "000001", true, exchange_time(32U)), 4U);
    send_one(&processor, order(5U, 5U, "000001", false, exchange_time(33U)), 5U);
    send_one(&processor, execution(6U, 6U, "000001", 4U, 5U,
                                  exchange_time(131U)), 6U);
    assert(samples.size() == 1U);
    std::int64_t expected_exchange_time = 0;
    assert(mix153060::parse_exchange_time_us(
               "09:32:11.000", 20260720, &expected_exchange_time));
    assert(samples[0].sample.exchange_time_us == expected_exchange_time);
}

void test_duplicate_and_gap() {
    const std::vector<mix153060::StaticInputs> configured(1, inputs("000001"));
    const std::vector<unsigned char> first = order(1U, 1U, "000001", true,
                                                   exchange_time(0));
    EventBytes first_event(first, 1U, 1700000000000000000ULL);
    sze_stream::SzeStreamProcessor duplicate(
        configured, 0, 1, sze_stream::SampleCallback());
    duplicate.on_event(first_event.event);
    duplicate.on_event(first_event.event);
    assert(duplicate.available() && duplicate.stats().duplicates == 1U);

    const std::vector<unsigned char> gap_bytes = order(3U, 2U, "000001", false,
                                                       exchange_time(1));
    EventBytes gap_event(gap_bytes, 2U, 1700000000000001000ULL);
    sze_stream::SzeStreamProcessor gap(
        configured, 0, 1, sze_stream::SampleCallback());
    gap.on_event(first_event.event);
    bool threw = false;
    try {
        gap.on_event(gap_event.event);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && !gap.available() && gap.error().find("gap") != std::string::npos);
}

void test_model_omission_is_explicit() {
    const std::vector<mix153060::StaticInputs> configured(1, inputs("000001"));
    bool threw = false;
    mix153060::Model unloaded;
    try {
        sze_stream::SzeStreamProcessor processor(
            configured, &unloaded, 1, sze_stream::SampleCallback());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

void test_validation_and_callback_failure() {
    std::vector<mix153060::StaticInputs> bad_symbol(1, inputs("000001.SH"));
    bool threw = false;
    try {
        sze_stream::SzeStreamProcessor processor(
            bad_symbol, 0, 1, sze_stream::SampleCallback());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::vector<mix153060::StaticInputs> bad_date(1, inputs("000001"));
    bad_date[0].trading_date = 20260721;
    threw = false;
    try {
        std::vector<mix153060::StaticInputs> configured;
        configured.push_back(inputs("000001"));
        configured.push_back(bad_date[0]);
        sze_stream::SzeStreamProcessor processor(
            configured, 0, 1, sze_stream::SampleCallback());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    unsigned char payload = 1U;
    StreamEvent idle = {};
    idle.kind = deepwin_market_data::kIdleEvent;
    idle.data = &payload;
    idle.size = 1U;
    sze_stream::SzeStreamProcessor idle_processor(
        std::vector<mix153060::StaticInputs>(1, inputs("000001")),
        0, 1, sze_stream::SampleCallback());
    threw = false;
    try {
        idle_processor.on_event(idle);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && !idle_processor.available());

    const std::vector<mix153060::StaticInputs> configured(1, inputs("000001"));
    std::vector<unsigned char> combined = order(1U, 1U, "000001", true,
                                                exchange_time(0));
    const std::vector<unsigned char> second = order(2U, 2U, "000001", false,
                                                    exchange_time(1));
    const std::vector<unsigned char> third = execution(
        3U, 3U, "000001", 1U, 2U, exchange_time(131U));
    combined.insert(combined.end(), second.begin(), second.end());
    combined.insert(combined.end(), third.begin(), third.end());
    sze_stream::SzeStreamProcessor callback_processor(
        configured, 0, 1,
        [&](const sze_stream::ProcessedSample&) {
            throw std::runtime_error("consumer failure");
        });
    EventBytes event(combined, 8U, 1700000000000000000ULL);
    threw = false;
    try {
        callback_processor.on_event(event.event);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && !callback_processor.available() &&
           callback_processor.error().find("callback") != std::string::npos);
    threw = false;
    try {
        callback_processor.on_event(event.event);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

void test_deferred_market_order_cancel_is_consumed() {
    mix153060::Runtime runtime(inputs("000001"));
    mix153060::SampleBuffer samples;
    const std::int64_t time = 34200000000LL;

    mix153060::OrderEvent market;
    market.app_sequence = 1;
    market.exchange_time_us = time;
    market.local_time_us = time;
    market.price = 0.0;
    market.volume = 100;
    market.buy = true;
    market.kind = mix153060::OrderKind::kMarket;
    runtime.on_order(market, &samples);
    assert(runtime.available() && samples.count == 0);

    mix153060::TradeEvent cancel;
    cancel.app_sequence = 2;
    cancel.exchange_time_us = time + 1;
    cancel.local_time_us = time + 1;
    cancel.price = 0.0;
    cancel.volume = 100;
    cancel.buy_order_id = 1;
    cancel.sell_order_id = 0;
    cancel.kind = mix153060::TradeKind::kCancel;
    runtime.on_trade(cancel, &samples);
    assert(runtime.available() && samples.count == 0);

    mix153060::OrderEvent bid = market;
    bid.app_sequence = 3;
    bid.exchange_time_us = time + 2;
    bid.local_time_us = time + 2;
    bid.price = 10.0;
    bid.kind = mix153060::OrderKind::kLimit;
    runtime.on_order(bid, &samples);
    mix153060::OrderEvent ask = bid;
    ask.app_sequence = 4;
    ask.exchange_time_us = time + 3;
    ask.local_time_us = time + 3;
    ask.price = 10.01;
    ask.buy = false;
    runtime.on_order(ask, &samples);
    mix153060::TradeEvent fill = cancel;
    fill.app_sequence = 5;
    fill.exchange_time_us = time + 4;
    fill.local_time_us = time + 4;
    fill.price = 10.0;
    fill.buy_order_id = 3;
    fill.sell_order_id = 4;
    fill.kind = mix153060::TradeKind::kFill;
    runtime.on_trade(fill, &samples);
    assert(runtime.available());
}

}  // namespace

int main() {
    test_live_replay_and_isolation();
    test_market_data_stream_capture_replay();
    test_control_records_preserve_continuity();
    test_concatenated_record_provenance();
    test_first_post_open_event_is_boundary_only();
    test_duplicate_and_gap();
    test_model_omission_is_explicit();
    test_validation_and_callback_failure();
    test_deferred_market_order_cancel_is_consumed();
    std::cout << "sze_stream_processor_test: PASS\n";
    return 0;
}
