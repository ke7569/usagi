#include "sze/market_data/SZEProtocol.h"
#include "sze/market_data/SZERecoverable.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

std::uint64_t now_ns()
{
    return sze_recovery::monotonic_time_ns();
}

void make_order(std::uint64_t id, sze_md::SzeHpfOrder* order)
{
    std::memset(order, 0, sizeof(*order));
    order->head.sequence = static_cast<std::uint32_t>(id);
    order->head.message_type = sze_md::kOrderMessage;
    order->head.security_type = 1U;
    order->head.sub_security_type = 0U;
    std::memcpy(order->head.symbol, "000001", 6U);
    order->head.exchange_id = 101U;
    const std::uint64_t time_of_day_ms =
        9U * 3600000ULL + 15U * 60000ULL + (id % (4U * 3600000ULL));
    const std::uint64_t hour = time_of_day_ms / 3600000ULL;
    const std::uint64_t minute = (time_of_day_ms / 60000ULL) % 60U;
    const std::uint64_t second = (time_of_day_ms / 1000ULL) % 60U;
    const std::uint64_t millisecond = time_of_day_ms % 1000U;
    order->head.quote_update_time = 20260722ULL * 1000000000ULL +
        hour * 10000000ULL + minute * 100000ULL + second * 1000ULL +
        millisecond;
    order->head.channel_num = 1U;
    order->head.sequence_num = id;
    order->head.md_stream_id = 1U;
    order->order_price = 107800U;
    order->order_quantity = 100U;
    order->side_flag = (id & 1U) ? '1' : '2';
    order->order_type = '2';
}

void remove_segments(const sze_recovery::JournalConfig& config,
                     std::uint32_t max_segments)
{
    for (std::uint32_t index = 0U; index < max_segments; ++index) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%s_%08u_s%u_%06u.szej",
                      config.directory.c_str(), config.prefix.c_str(),
                      config.trading_day, config.source_id, index);
        (void)::unlink(path);
    }
    (void)::rmdir(config.directory.c_str());
}

}  // namespace

int main(int argc, char** argv)
{
    std::uint64_t event_count = 1000000U;
    if (argc == 2) {
        char* end = 0;
        event_count = std::strtoull(argv[1], &end, 10);
        if (!end || *end != '\0' || event_count == 0U) {
            std::cerr << "usage: sze_recovery_benchmark [EVENT_COUNT]" << std::endl;
            return 2;
        }
    } else if (argc != 1) {
        std::cerr << "usage: sze_recovery_benchmark [EVENT_COUNT]" << std::endl;
        return 2;
    }

    char directory_template[128];
    std::snprintf(directory_template, sizeof(directory_template),
                  "/tmp/sze_recovery_benchmark_%ld_XXXXXX",
                  static_cast<long>(::getpid()));
    char* directory_value = ::mkdtemp(directory_template);
    if (!directory_value) {
        std::cerr << "cannot create benchmark directory" << std::endl;
        return 3;
    }

    sze_recovery::JournalConfig config;
    config.directory = directory_value;
    config.prefix = "benchmark";
    config.trading_day = 20260722U;
    config.source_id = 88U;
    config.segment_bytes = 16U << 20U;
    config.max_payload_bytes = sizeof(sze_md::SzeHpfOrder);
    config.min_free_bytes_after_allocate = 0U;
    const std::uint32_t max_segments = static_cast<std::uint32_t>(
        event_count / 80000U + 8U);

    sze_recovery::JournalWriter writer;
    if (writer.open(config).status != sze_recovery::kJournalOk) {
        remove_segments(config, max_segments);
        return 4;
    }
    unsigned char payload[sizeof(sze_md::SzeHpfOrder)];
    const std::uint64_t write_begin = now_ns();
    for (std::uint64_t id = 1U; id <= event_count; ++id) {
        sze_md::SzeHpfOrder order;
        make_order(id, &order);
        std::memcpy(payload, &order, sizeof(order));
        sze_recovery::CanonicalEvent event;
        std::memset(&event, 0, sizeof(event));
        event.feed_sequence = id;
        event.channel_sequence = id;
        event.receive_mono_ns = id * 100U;
        event.exchange_time = order.head.quote_update_time;
        event.trading_day = config.trading_day;
        event.source_id = static_cast<std::uint16_t>(config.source_id);
        event.channel_number = order.head.channel_num;
        event.payload_size = static_cast<std::uint16_t>(sizeof(order));
        event.message_type = order.head.message_type;
        event.record_kind = sze_recovery::kRecordMarketData;
        if (writer.append(&event, payload) != sze_recovery::kJournalOk) {
            std::cerr << "journal append failed event_id=" << id << std::endl;
            writer.close(false);
            remove_segments(config, max_segments);
            return 5;
        }
    }
    if (writer.close(true) != sze_recovery::kJournalOk) {
        remove_segments(config, max_segments);
        return 6;
    }
    const std::uint64_t write_end = now_ns();

    sze_recovery::JournalReader reader;
    if (reader.open(config).status != sze_recovery::kJournalOk) {
        remove_segments(config, max_segments);
        return 7;
    }
    std::uint64_t decoded = 0U;
    std::uint64_t orders = 0U;
    const std::uint64_t replay_begin = now_ns();
    for (;;) {
        sze_recovery::CanonicalEvent event;
        const sze_recovery::JournalStatus status = reader.next(
            &event, payload, sizeof(payload));
        if (status == sze_recovery::kJournalEnd ||
            status == sze_recovery::kJournalWouldBlock) {
            break;
        }
        if (status != sze_recovery::kJournalOk) {
            std::cerr << "journal read failed event_id=" << reader.next_event_id()
                      << std::endl;
            reader.close();
            remove_segments(config, max_segments);
            return 8;
        }
        LFL2OrderField order;
        LFL2TradeField trade;
        std::memset(&order, 0, sizeof(order));
        std::memset(&trade, 0, sizeof(trade));
        if (sze_md::decode_recovery_record(
                event, payload, event.payload_size, &order, &trade) !=
            sze_md::DecodeStatus::kOrder) {
            std::cerr << "decode failed event_id=" << event.event_id << std::endl;
            reader.close();
            remove_segments(config, max_segments);
            return 9;
        }
        ++decoded;
        ++orders;
    }
    const std::uint64_t replay_end = now_ns();
    reader.close();
    remove_segments(config, max_segments);

    const double write_seconds =
        static_cast<double>(write_end - write_begin) / 1e9;
    const double replay_seconds =
        static_cast<double>(replay_end - replay_begin) / 1e9;
    const double replay_rate = replay_seconds > 0.0
        ? static_cast<double>(decoded) / replay_seconds : 0.0;
    std::cout << "sze_recovery_benchmark status=PASS"
              << " events=" << event_count
              << " decoded=" << decoded
              << " orders=" << orders
              << " write_seconds=" << write_seconds
              << " replay_seconds=" << replay_seconds
              << " replay_events_per_second=" << replay_rate
              << " ten_minute_capacity=" << replay_rate * 600.0
              << std::endl;
    return decoded == event_count ? 0 : 10;
}
