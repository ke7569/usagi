#include "sze/market_data/SZEProtocol.h"
#include "sze/market_data/SZERecoverable.h"

#include <stdexcept>
#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(#expression); } while (0)
#include <cstring>
#include <iostream>
#include <signal.h>
#include <string>
#include <time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void stop_signal(int) { g_stop = 1; }

std::uint32_t china_day() {
    const std::time_t now = std::time(0);
    std::tm value = {};
    if (::gmtime_r(&now, &value) == 0) return 0U;
    const std::time_t china_seconds = now + 8 * 3600;
    if (::gmtime_r(&china_seconds, &value) == 0) return 0U;
    return static_cast<std::uint32_t>(value.tm_year + 1900) * 10000U +
        static_cast<std::uint32_t>(value.tm_mon + 1) * 100U +
        static_cast<std::uint32_t>(value.tm_mday);
}

void set_symbol(std::uint8_t* destination) {
    std::memset(destination, 0, 9U);
    std::memcpy(destination, "000001", 6U);
}

std::uint64_t exchange_time_for_day(std::uint32_t day,
                                    std::uint64_t milliseconds_after_open) {
    const std::uint64_t total_ms = 9U * 3600000U + 30U * 60000U +
                                   milliseconds_after_open;
    return static_cast<std::uint64_t>(day) * 1000000000ULL +
        (total_ms / 3600000U) * 10000000ULL +
        ((total_ms / 60000U) % 60U) * 100000ULL +
        ((total_ms / 1000U) % 60U) * 1000ULL +
        (total_ms % 1000U);
}

std::uint64_t exchange_time(std::uint64_t milliseconds_after_open) {
    return exchange_time_for_day(20260904U, milliseconds_after_open);
}

sze_md::SzeHpfHead head(std::uint8_t type, std::uint32_t feed,
                        std::uint64_t application, std::uint64_t time) {
    sze_md::SzeHpfHead value = {};
    value.sequence = feed;
    value.message_type = type;
    value.security_type = 1U;
    set_symbol(value.symbol);
    value.exchange_id = 101U;
    value.quote_update_time = time;
    value.channel_num = 7U;
    value.sequence_num = application;
    value.md_stream_id = 1U;
    return value;
}

std::vector<unsigned char> order(std::uint32_t feed, std::uint64_t application,
                                 bool buy, std::uint64_t time) {
    sze_md::SzeHpfOrder value = {};
    value.head = head(sze_md::kOrderMessage, feed, application, time);
    value.order_price = 100000U;
    value.order_quantity = 10000U;
    value.side_flag = buy ? '1' : '2';
    value.order_type = '2';
    return std::vector<unsigned char>(
        reinterpret_cast<unsigned char*>(&value),
        reinterpret_cast<unsigned char*>(&value) + sizeof(value));
}

std::vector<unsigned char> execution(std::uint32_t feed,
                                     std::uint64_t application,
                                     std::int64_t buy_id,
                                     std::int64_t sell_id,
                                     std::uint64_t time) {
    sze_md::SzeHpfExecution value = {};
    value.head = head(sze_md::kExecutionMessage, feed, application, time);
    value.trade_buy_num = buy_id;
    value.trade_sell_num = sell_id;
    value.trade_price = 100000U;
    value.trade_quantity = 10000;
    value.trade_type = 'F';
    return std::vector<unsigned char>(
        reinterpret_cast<unsigned char*>(&value),
        reinterpret_cast<unsigned char*>(&value) + sizeof(value));
}

void append(sze_recovery::JournalWriter* writer, std::uint64_t feed,
            std::uint64_t application, const std::vector<unsigned char>& payload,
            std::uint64_t time, std::uint32_t day) {
    sze_recovery::CanonicalEvent event = {};
    event.feed_sequence = feed;
    event.channel_sequence = application;
    event.receive_mono_ns = application * 1000U;
    event.exchange_time = time;
    event.trading_day = day;
    event.source_id = 88U;
    event.channel_number = 7U;
    event.payload_size = static_cast<std::uint16_t>(payload.size());
    event.message_type = payload[8];
    event.record_kind = sze_recovery::kRecordMarketData;
    CHECK(writer->append(&event, payload.data()) == sze_recovery::kJournalOk);
}

int handoff_fixture(const std::string& directory) {
    const std::uint32_t day = china_day();
    if (day == 0U) return 3;
    sze_recovery::JournalConfig config;
    config.directory = directory;
    config.prefix = "fixture";
    config.trading_day = day;
    config.source_id = 88U;
    config.segment_bytes = 1U << 20U;
    config.max_payload_bytes = 256U;
    sze_recovery::JournalWriter writer;
    if (writer.open(config).status != sze_recovery::kJournalOk) return 4;
    const std::string shm_path = directory + "/handoff.shm";
    sze_recovery::RingConfig ring_config;
    ring_config.path = shm_path;
    ring_config.trading_day = day;
    ring_config.source_id = 88U;
    ring_config.capacity = 256U;
    ring_config.max_payload_bytes = 256U;
    ring_config.generation = writer.generation();
    sze_recovery::ShmEventRing producer;
    if (!producer.create(ring_config)) return 5;
    producer.publish_state(sze_recovery::kContinuityValid,
                           sze_recovery::kReadinessNotReady,
                           sze_recovery::kInvalidNone, 0U, 0U);
    ::signal(SIGTERM, stop_signal);
    ::signal(SIGINT, stop_signal);
    std::uint64_t application = 1U;
    std::uint64_t feed = 100U;
    std::uint64_t milliseconds = 0U;
    const auto publish = [&]() {
        const bool buy = (application & 1U) != 0U;
        const std::uint64_t time = exchange_time_for_day(day, milliseconds);
        const std::vector<unsigned char> payload = order(
            static_cast<std::uint32_t>(feed), application, buy, time);
        sze_recovery::CanonicalEvent event = {};
        event.feed_sequence = feed;
        event.channel_sequence = application;
        event.receive_mono_ns = sze_recovery::monotonic_time_ns();
        event.exchange_time = time;
        event.trading_day = day;
        event.source_id = 88U;
        event.channel_number = 7U;
        event.payload_size = static_cast<std::uint16_t>(payload.size());
        event.message_type = payload[8];
        event.record_kind = sze_recovery::kRecordMarketData;
        if (writer.append(&event, payload.data()) != sze_recovery::kJournalOk ||
            !producer.publish(event, payload.data())) return false;
        ++application;
        ++feed;
        milliseconds = (milliseconds + 1U) % 300000U;
        return true;
    };
    if (!publish()) return 6;
    const std::uint64_t generation = writer.generation();
    std::cout << "{\"mode\":\"handoff\",\"directory\":\"" << directory
              << "\",\"shm_path\":\"" << shm_path
              << "\",\"prefix\":\"fixture\",\"day\":" << day
              << ",\"source\":88,\"generation\":" << generation
              << ",\"segment_bytes\":1048576,\"payload_max\":256}\n";
    std::cout.flush();
    while (!g_stop) {
        if (!publish()) return 7;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    writer.publish_continuity(sze_recovery::kContinuityValid,
                              sze_recovery::kInvalidNone, feed - 1U);
    writer.close(true);
    producer.close();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--handoff")
        return handoff_fixture(argv[2]);
    if (argc != 2) {
        std::cerr << "usage: sze_recovery_runtime_fixture DIRECTORY\n";
        return 2;
    }
    sze_recovery::JournalConfig config;
    config.directory = argv[1];
    config.prefix = "fixture";
    config.trading_day = 20260904U;
    config.source_id = 88U;
    config.segment_bytes = 1U << 20U;
    config.max_payload_bytes = 256U;
    sze_recovery::JournalWriter writer;
    const sze_recovery::JournalOpenResult opened = writer.open(config);
    if (opened.status != sze_recovery::kJournalOk) return 3;
    append(&writer, 100U, 1U, order(100U, 1U, true, exchange_time(0U)),
           exchange_time(0U), 20260904U);
    append(&writer, 102U, 2U, order(102U, 2U, false, exchange_time(1U)),
           exchange_time(1U), 20260904U);
    append(&writer, 105U, 3U, execution(105U, 3U, 1, 2, exchange_time(30U)),
           exchange_time(30U), 20260904U);
    append(&writer, 106U, 4U, order(106U, 4U, true, exchange_time(32U)),
           exchange_time(32U), 20260904U);
    append(&writer, 108U, 5U, order(108U, 5U, false, exchange_time(33U)),
           exchange_time(33U), 20260904U);
    append(&writer, 111U, 6U, execution(111U, 6U, 4, 5, exchange_time(131000U)),
           exchange_time(131000U), 20260904U);
    const std::uint64_t generation = writer.generation();
    if (writer.publish_continuity(sze_recovery::kContinuityValid,
                                  sze_recovery::kInvalidNone, 111U) !=
            sze_recovery::kJournalOk ||
        writer.close(true) != sze_recovery::kJournalOk)
        return 4;
    std::cout << "{\"directory\":\"" << config.directory
              << "\",\"prefix\":\"fixture\",\"day\":20260904"
              << ",\"source\":88,\"generation\":" << generation
              << ",\"segment_bytes\":1048576,\"payload_max\":256}\n";
    return 0;
}
