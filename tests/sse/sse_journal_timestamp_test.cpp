#include "sse/runtime/sse_journal_transport.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void expect_throw(Function function, const char* message) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, message);
}

std::string temporary_directory() {
    char path[] = "/tmp/sse_timestamp_test_XXXXXX";
    char* result = ::mkdtemp(path);
    check(result != 0, "cannot create timestamp test directory");
    return result;
}

sse_journal::Config config(bool extended, const std::string& directory) {
    sse_journal::Config value;
    value.extended_timestamps = extended;
    value.journal.directory = directory;
    value.journal.prefix = "timestamp";
    value.journal.trading_day = 20260908U;
    value.journal.source_id = 89U;
    value.journal.segment_bytes = 32768U;
    value.journal.max_payload_bytes =
        static_cast<std::uint32_t>(sse_journal::stored_header_bytes(value) +
                                    sse_journal::kMaxDatagram);
    value.journal.generation = 0x87654321U;
    value.ring.path = directory + "/events.shm";
    value.ring.trading_day = value.journal.trading_day;
    value.ring.source_id = value.journal.source_id;
    value.ring.capacity = 4U;
    value.ring.max_payload_bytes = value.journal.max_payload_bytes;
    value.ring.generation = value.journal.generation;
    value.channels.push_back(deepwin_market_data::ChannelSpec());
    value.channels[0].name = "timestamp";
    value.channels[0].group = "127.0.0.1";
    value.channels[0].interface_ip = "127.0.0.1";
    value.channels[0].port = 37189;
    return value;
}

deepwin_market_data::StreamEvent event(std::uint16_t flags) {
    static const unsigned char bytes[] = {0x3eU, 0x01U, 0xa5U, 0x5aU, 0x7fU};
    deepwin_market_data::StreamEvent value = {};
    value.kind = deepwin_market_data::kDatagramEvent;
    value.sequence = 17U;
    value.monotonic_ns = 1000000001ULL;
    value.realtime_ns = 2000000002ULL;
    value.receive_batch = 23U;
    value.channel_id = 0U;
    value.batch_index = 2U;
    value.batch_size = 3U;
    value.source_ipv4 = 0x7f000001U;
    value.source_port = 37189U;
    value.timestamp_flags = flags;
    value.data = bytes;
    value.size = sizeof(bytes);
    // Deliberately nonzero so a v1/no-request encode would expose an
    // accidental read of fields that older callers do not initialize.
    value.hardware_ns = 0x1111222233334444ULL;
    value.application_realtime_ns = 0x5555666677778888ULL;
    value.hardware_clock_index = 13;
    return value;
}

void check_old_fields(const deepwin_market_data::StreamEvent& actual,
                      std::uint16_t flags,
                      std::uint64_t application_realtime_ns) {
    check(actual.kind == deepwin_market_data::kDatagramEvent, "kind changed");
    check(actual.sequence == 1U, "sequence did not use canonical event id");
    check(actual.monotonic_ns == 1000000001ULL, "monotonic timestamp changed");
    check(actual.realtime_ns == 2000000002ULL, "realtime timestamp changed");
    check(actual.receive_batch == 23U && actual.channel_id == 0U,
          "batch/channel metadata changed");
    check(actual.batch_index == 2U && actual.batch_size == 3U,
          "batch position changed");
    check(actual.source_ipv4 == 0x7f000001U && actual.source_port == 37189U,
          "source metadata changed");
    check(actual.timestamp_flags == flags && actual.size == 5U,
          "payload metadata changed");
    check(actual.application_realtime_ns == application_realtime_ns,
          "software application timestamp changed");
    static const unsigned char expected[] = {0x3eU, 0x01U, 0xa5U, 0x5aU, 0x7fU};
    check(std::memcmp(actual.data, expected, sizeof(expected)) == 0,
          "payload bytes changed");
}

void test_v1_byte_compatibility(const std::string& directory) {
    const sse_journal::Config value = config(false, directory);
    check(sizeof(sse_journal::PayloadHeader) == 48U,
          "v1 payload header ABI changed");
    check(sse_journal::stored_header_bytes(value) == 48U,
          "v1 header size changed");
    deepwin_market_data::StreamEvent input = event(
        deepwin_market_data::kKernelRealtimeTimestamp);
    sse_journal::StoredEvent stored;
    sse_journal::encode(input, value, &stored);
    check(stored.event.payload_size == 53U, "v1 payload size changed");

    sse_journal::PayloadHeader expected = {};
    expected.magic = sse_journal::kPayloadMagic;
    expected.version = sse_journal::kPayloadVersion;
    expected.kind = deepwin_market_data::kDatagramEvent;
    expected.realtime_ns = input.realtime_ns;
    expected.receive_batch = input.receive_batch;
    expected.channel_id = input.channel_id;
    expected.batch_index = input.batch_index;
    expected.batch_size = input.batch_size;
    expected.source_ipv4 = input.source_ipv4;
    expected.source_port = input.source_port;
    expected.timestamp_flags = input.timestamp_flags;
    expected.payload_bytes = static_cast<std::uint32_t>(input.size);
    check(std::memcmp(stored.payload, &expected, sizeof(expected)) == 0,
          "v1 header is not byte-compatible");
    check(std::memcmp(stored.payload + sizeof(expected), input.data, input.size) == 0,
          "v1 payload bytes changed");

    // JournalWriter assigns the first canonical event id; decode must expose
    // that id while retaining all source metadata.
    stored.event.event_id = 1U;
    stored.event.trading_day = value.journal.trading_day;
    stored.event.source_id = 89U;
    stored.event.record_kind = sze_recovery::kRecordMarketData;
    const deepwin_market_data::StreamEvent output = sse_journal::decode(
        stored.event, stored.payload, value);
    check_old_fields(output, input.timestamp_flags, 0U);
    check(output.hardware_ns == 0U && output.hardware_clock_index == -1,
          "v1 hardware timestamp was not absent");
}

void test_v2_timestamps(const std::string& directory) {
    sse_journal::Config value = config(true, directory);
    value.stream.hardware_timestamp_interface = "eth-test";
    check(sizeof(sse_journal::PayloadHeaderV2) == 80U,
          "v2 payload header ABI changed");
    check(sse_journal::stored_header_bytes(value) == 80U,
          "v2 header size changed");

    deepwin_market_data::StreamEvent input = event(
        static_cast<std::uint16_t>(deepwin_market_data::kKernelRealtimeTimestamp |
                                   deepwin_market_data::kHardwareTimestampRequested |
                                   deepwin_market_data::kHardwareReceiveTimestamp));
    input.hardware_ns = 0x0102030405060708ULL;
    input.application_realtime_ns = 0x1112131415161718ULL;
    input.hardware_clock_index = 7;
    sse_journal::StoredEvent stored;
    sse_journal::encode(input, value, &stored);
    check(stored.event.payload_size == 85U, "v2 payload size changed");
    sse_journal::PayloadHeaderV2 extended = {};
    std::memcpy(&extended, stored.payload, sizeof(extended));
    check(extended.prefix.version == sse_journal::kPayloadVersionV2,
          "v2 prefix version missing");
    check(extended.hardware_ns == input.hardware_ns &&
          extended.application_realtime_ns == input.application_realtime_ns &&
          extended.hardware_clock_index == input.hardware_clock_index,
          "v2 hardware timestamps were not stored");
    check(extended.reserved == 0U && extended.reserved2 == 0U,
          "v2 reserved fields are not zero");
    stored.event.event_id = 1U;
    stored.event.trading_day = value.journal.trading_day;
    stored.event.source_id = 89U;
    stored.event.record_kind = sze_recovery::kRecordMarketData;
    deepwin_market_data::StreamEvent output = sse_journal::decode(
        stored.event, stored.payload, value);
    check_old_fields(output, input.timestamp_flags, input.application_realtime_ns);
    check(output.hardware_ns == input.hardware_ns &&
          output.application_realtime_ns == input.application_realtime_ns &&
          output.hardware_clock_index == input.hardware_clock_index,
          "v2 hardware timestamps were not decoded");

    // A v2 software-only stream still has an explicit extension, but must not
    // claim a hardware clock or read uninitialized extension fields.
    deepwin_market_data::StreamEvent software = event(
        deepwin_market_data::kUserspaceRealtimeTimestamp);
    sse_journal::StoredEvent software_stored;
    sse_journal::encode(software, value, &software_stored);
    std::memcpy(&extended, software_stored.payload, sizeof(extended));
    check(extended.hardware_ns == 0U && extended.hardware_clock_index == -1 &&
          extended.application_realtime_ns == 0U,
          "v2 software-only extension claims hardware");
    software_stored.event.event_id = 1U;
    software_stored.event.trading_day = value.journal.trading_day;
    software_stored.event.source_id = 89U;
    software_stored.event.record_kind = sze_recovery::kRecordMarketData;
    output = sse_journal::decode(software_stored.event,
                                 software_stored.payload, value);
    check_old_fields(output, software.timestamp_flags, 0U);
    check(output.hardware_ns == 0U && output.hardware_clock_index == -1,
          "v2 software-only extension claims hardware");

    deepwin_market_data::StreamEvent idle = event(
        deepwin_market_data::kUserspaceRealtimeTimestamp);
    idle.kind = deepwin_market_data::kIdleEvent;
    idle.data = 0;
    idle.size = 0U;
    sse_journal::StoredEvent idle_stored;
    sse_journal::encode(idle, value, &idle_stored);
    std::memcpy(&extended, idle_stored.payload, sizeof(extended));
    check(extended.hardware_ns == 0U && extended.hardware_clock_index == -1 &&
          extended.application_realtime_ns == idle.realtime_ns,
          "v2 idle application timestamp was not retained");
    idle_stored.event.event_id = 1U;
    idle_stored.event.trading_day = value.journal.trading_day;
    idle_stored.event.source_id = 89U;
    idle_stored.event.record_kind = sze_recovery::kRecordMarketData;
    output = sse_journal::decode(idle_stored.event, idle_stored.payload, value);
    check(output.kind == deepwin_market_data::kIdleEvent &&
          output.application_realtime_ns == idle.realtime_ns &&
          output.hardware_ns == 0U && output.hardware_clock_index == -1,
          "v2 idle timestamp round-trip changed");
}

void test_rejections(const std::string& directory) {
    const sse_journal::Config v1 = config(false, directory);
    sse_journal::Config v2 = config(true, directory);
    deepwin_market_data::StreamEvent requested = event(
        deepwin_market_data::kHardwareTimestampRequested);
    requested.hardware_ns = 0U;
    requested.application_realtime_ns = 0x5555666677778888ULL;
    requested.hardware_clock_index = 13;
    sse_journal::StoredEvent stored;
    sse_journal::encode(requested, v2, &stored);
    stored.event.event_id = 1U;
    stored.event.trading_day = v2.journal.trading_day;
    stored.event.source_id = 89U;
    stored.event.record_kind = sze_recovery::kRecordMarketData;

    expect_throw([&]() {
        deepwin_market_data::StreamEvent unused = sse_journal::decode(
            stored.event, stored.payload, v1);
        (void)unused;
    }, "v1 decoder accepted v2 payload");

    sse_journal::PayloadHeaderV2 extended = {};
    std::memcpy(&extended, stored.payload, sizeof(extended));
    extended.prefix.version = sse_journal::kPayloadVersion;
    std::memcpy(stored.payload, &extended, sizeof(extended));
    expect_throw([&]() {
        deepwin_market_data::StreamEvent unused = sse_journal::decode(
            stored.event, stored.payload, v2);
        (void)unused;
    }, "v2 decoder accepted a v1 prefix");

    sse_journal::encode(requested, v2, &stored);
    stored.event.event_id = 1U;
    stored.event.trading_day = v2.journal.trading_day;
    stored.event.source_id = 89U;
    stored.event.record_kind = sze_recovery::kRecordMarketData;
    std::memcpy(&extended, stored.payload, sizeof(extended));
    extended.prefix.timestamp_flags = deepwin_market_data::kHardwareReceiveTimestamp;
    std::memcpy(stored.payload, &extended, sizeof(extended));
    expect_throw([&]() {
        deepwin_market_data::StreamEvent unused = sse_journal::decode(
            stored.event, stored.payload, v2);
        (void)unused;
    }, "decoder accepted hardware-received without request flag");

    sse_journal::encode(requested, v2, &stored);
    stored.event.event_id = 1U;
    stored.event.trading_day = v2.journal.trading_day;
    stored.event.source_id = 89U;
    stored.event.record_kind = sze_recovery::kRecordMarketData;
    std::memcpy(&extended, stored.payload, sizeof(extended));
    extended.prefix.timestamp_flags = static_cast<std::uint16_t>(
        deepwin_market_data::kHardwareTimestampRequested |
        deepwin_market_data::kHardwareReceiveTimestamp);
    extended.hardware_ns = 0U;
    std::memcpy(stored.payload, &extended, sizeof(extended));
    expect_throw([&]() {
        deepwin_market_data::StreamEvent unused = sse_journal::decode(
            stored.event, stored.payload, v2);
        (void)unused;
    }, "decoder accepted hardware-received flag without timestamp");

    sse_journal::encode(requested, v2, &stored);
    stored.event.event_id = 1U;
    stored.event.trading_day = v2.journal.trading_day;
    stored.event.source_id = 89U;
    stored.event.record_kind = sze_recovery::kRecordMarketData;
    std::memcpy(&extended, stored.payload, sizeof(extended));
    extended.prefix.timestamp_flags = 0x10U;
    std::memcpy(stored.payload, &extended, sizeof(extended));
    expect_throw([&]() {
        deepwin_market_data::StreamEvent unused = sse_journal::decode(
            stored.event, stored.payload, v2);
        (void)unused;
    }, "decoder accepted unknown timestamp flag");

    deepwin_market_data::StreamEvent received_without_request = event(
        deepwin_market_data::kHardwareReceiveTimestamp);
    expect_throw([&]() { sse_journal::encode(received_without_request, v2, &stored); },
                 "encoder accepted hardware-received without request flag");

    deepwin_market_data::StreamEvent old_request = event(
        deepwin_market_data::kHardwareTimestampRequested);
    expect_throw([&]() { sse_journal::encode(old_request, v1, &stored); },
                 "v1 encoder accepted hardware timestamp fields");

    std::memcpy(&extended, stored.payload, sizeof(extended));
    extended.prefix.version = sse_journal::kPayloadVersionV2;
    std::memcpy(stored.payload, &extended, sizeof(extended));
    stored.event.payload_size = static_cast<std::uint16_t>(
        sizeof(sse_journal::PayloadHeader) + old_request.size);
    expect_throw([&]() {
        deepwin_market_data::StreamEvent unused = sse_journal::decode(
            stored.event, stored.payload, v2);
        (void)unused;
    }, "v2 decoder accepted truncated extension");
}

nlohmann::json json_config(const std::string& directory,
                           const std::string& format,
                           const std::string& hardware_interface,
                           const std::string& boot) {
    nlohmann::json value;
    value["schema_version"] = 1;
    value["payload_format"] = format;
    value["trading_day"] = 20260908;
    value["source_id"] = 89;
    value["generation"] = 9;
    value["boot_id"] = boot;
    value["journal_directory"] = directory;
    value["journal_prefix"] = "load";
    value["shm_path"] = directory + "/load.shm";
    value["segment_bytes"] = 32768;
    value["min_free_bytes_after_allocate"] = 0;
    value["ring_capacity"] = 4;
    value["journal_queue_capacity"] = 4;
    value["queue_capacity"] = 4;
    value["max_datagram_bytes"] = 8192;
    value["receive_batch_size"] = 4;
    value["receive_buffer_bytes"] = 1048576;
    value["idle_gap_ns"] = 100000;
    value["receive_cpu"] = -1;
    value["dispatch_cpu"] = -1;
    value["journal_cpu"] = -1;
    value["prediction_cpu"] = -1;
    value["flush_interval_ms"] = 1;
    value["duration_ms"] = 0;
    if (!hardware_interface.empty()) value["hardware_timestamp_interface"] = hardware_interface;
    value["channels"] = nlohmann::json::array();
    value["channels"].push_back({
        {"name", "load"}, {"group", "127.0.0.1"},
        {"port", 37190}, {"interface_ip", "127.0.0.1"}});
    return value;
}

void test_config_load(const std::string& directory) {
    const std::string boot = sse_journal::boot_id();
    const std::string path = directory + "/config.json";
    {
        std::ofstream output(path.c_str());
        check(output.good(), "cannot write timestamp config fixture");
        output << json_config(directory, "sse-stream-v1", "", boot).dump();
    }
    sse_journal::Config v1 = sse_journal::load(path);
    check(!v1.extended_timestamps && sse_journal::stored_header_bytes(v1) == 48U &&
          v1.journal.max_payload_bytes == 8240U &&
          v1.stream.hardware_timestamp_interface.empty(),
          "v1 config load changed timestamp defaults");

    {
        std::ofstream output(path.c_str());
        output << json_config(directory, "sse-stream-v2", "eth-test", boot).dump();
    }
    sse_journal::Config v2 = sse_journal::load(path);
    check(v2.extended_timestamps && sse_journal::stored_header_bytes(v2) == 80U &&
          v2.journal.max_payload_bytes == 8272U &&
          v2.stream.hardware_timestamp_interface == "eth-test",
          "v2 config load did not enable extended timestamps");

    {
        std::ofstream output(path.c_str());
        output << json_config(directory, "sse-stream-v1", "eth-test", boot).dump();
    }
    expect_throw([&]() { (void)sse_journal::load(path); },
                 "v1 config accepted a hardware timestamp interface");
    (void)::unlink(path.c_str());
}

std::string journal_segment(const sse_journal::Config& value) {
    char path[512];
    const int written = std::snprintf(
        path, sizeof(path), "%s/%s_%08u_s%u_%06u.szej",
        value.journal.directory.c_str(), value.journal.prefix.c_str(),
        value.journal.trading_day, value.journal.source_id, 0U);
    check(written > 0 && static_cast<std::size_t>(written) < sizeof(path),
          "journal segment path overflow");
    return path;
}

void test_journal_and_shm(const std::string& directory) {
    sse_journal::Config value = config(true, directory);
    sze_recovery::JournalWriter writer;
    sze_recovery::ShmEventRing producer;
    sze_recovery::ShmEventRing consumer;
    sze_recovery::JournalReader reader;
    try {
        check(writer.open(value.journal).status == sze_recovery::kJournalOk,
              "v2 journal open failed");
        check(writer.publish_continuity(sze_recovery::kContinuityValid,
                                        sze_recovery::kInvalidNone, 0U) ==
                  sze_recovery::kJournalOk,
              "v2 journal continuity failed");
        check(producer.create(value.ring), "v2 SHM create failed");
        producer.publish_state(sze_recovery::kContinuityValid,
                               sze_recovery::kReadinessLiveReady,
                               sze_recovery::kInvalidNone, 0U, 0U);

        deepwin_market_data::StreamEvent input = event(
            static_cast<std::uint16_t>(deepwin_market_data::kKernelRealtimeTimestamp |
                                       deepwin_market_data::kHardwareTimestampRequested |
                                       deepwin_market_data::kHardwareReceiveTimestamp));
        input.sequence = 1U;
        input.hardware_ns = 0x9988776655443322ULL;
        input.application_realtime_ns = 0x2233445566778899ULL;
        input.hardware_clock_index = 4;
        sse_journal::StoredEvent stored;
        sse_journal::encode(input, value, &stored);
        sze_recovery::CanonicalEvent published = stored.event;
        check(writer.append(&published, stored.payload) == sze_recovery::kJournalOk,
              "v2 journal append failed");
        check(producer.publish(published, stored.payload), "v2 SHM publish failed");

        check(reader.open(value.journal).status == sze_recovery::kJournalOk,
              "v2 journal reader open failed");
        sze_recovery::CanonicalEvent journal_event;
        unsigned char journal_payload[sizeof(sse_journal::PayloadHeaderV2) +
                                     sse_journal::kMaxDatagram];
        check(reader.next(&journal_event, journal_payload, sizeof(journal_payload)) ==
                  sze_recovery::kJournalOk,
              "v2 journal reader returned no event");
        deepwin_market_data::StreamEvent journal_decoded = sse_journal::decode(
            journal_event, journal_payload, value);
        check(journal_decoded.hardware_ns == input.hardware_ns &&
              journal_decoded.application_realtime_ns == input.application_realtime_ns &&
              journal_decoded.hardware_clock_index == input.hardware_clock_index,
              "journal reader lost v2 timestamp fields");

        check(consumer.attach(value.ring.path), "v2 SHM attach failed");
        sze_recovery::CanonicalEvent shm_event;
        unsigned char shm_payload[sizeof(sse_journal::PayloadHeaderV2) +
                                  sse_journal::kMaxDatagram];
        check(consumer.read(1U, &shm_event, shm_payload, sizeof(shm_payload)) ==
                  sze_recovery::kRingReadOk,
              "v2 SHM reader returned no event");
        deepwin_market_data::StreamEvent shm_decoded = sse_journal::decode(
            shm_event, shm_payload, value);
        check(shm_decoded.hardware_ns == input.hardware_ns &&
              shm_decoded.application_realtime_ns == input.application_realtime_ns &&
              shm_decoded.hardware_clock_index == input.hardware_clock_index,
              "SHM reader lost v2 timestamp fields");

        reader.close();
        consumer.close();
        producer.close();
        check(writer.close(true) == sze_recovery::kJournalOk,
              "v2 journal clean close failed");
    } catch (...) {
        reader.close();
        consumer.close();
        producer.close();
        (void)writer.close(false);
        (void)::unlink(value.ring.path.c_str());
        (void)::unlink(journal_segment(value).c_str());
        throw;
    }
    (void)::unlink(value.ring.path.c_str());
    check(::unlink(journal_segment(value).c_str()) == 0,
          "v2 journal segment cleanup failed");
}

void run() {
    const std::string directory = temporary_directory();
    try {
        test_config_load(directory);
        test_v1_byte_compatibility(directory);
        test_v2_timestamps(directory);
        test_rejections(directory);
        test_journal_and_shm(directory);
    } catch (...) {
        (void)::unlink((directory + "/events.shm").c_str());
        (void)::unlink((directory + "/timestamp_20260908_s89_000000.szej").c_str());
        (void)::unlink((directory + "/config.json").c_str());
        (void)::rmdir(directory.c_str());
        throw;
    }
    check(::rmdir(directory.c_str()) == 0, "timestamp test directory cleanup failed");
}

}  // namespace

int main() {
    try {
        run();
        std::cout << "sse_journal_timestamp_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sse_journal_timestamp_test: " << error.what() << '\n';
        return 1;
    }
}
