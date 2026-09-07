#include "sze/market_data/SZEProtocol.h"
#include "sze/market_data/SZERecoverable.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool parse_u64(const char* text, std::uint64_t* output)
{
    if (!text || !output || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char* end = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *output = static_cast<std::uint64_t>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 6) {
        std::cerr << "usage: sze_recovery_verify DIRECTORY PREFIX TRADING_DAY SEGMENT_BYTES MAX_PAYLOAD_BYTES"
                  << std::endl;
        return 2;
    }
    std::uint64_t trading_day = 0U;
    std::uint64_t segment_bytes = 0U;
    std::uint64_t max_payload = 0U;
    if (!parse_u64(argv[3], &trading_day) ||
        !parse_u64(argv[4], &segment_bytes) ||
        !parse_u64(argv[5], &max_payload) ||
        trading_day > 99991231U || max_payload > 65535U) {
        std::cerr << "sze_recovery_verify invalid_arguments=1" << std::endl;
        return 2;
    }

    sze_recovery::JournalConfig config;
    config.directory = argv[1];
    config.prefix = argv[2];
    config.trading_day = static_cast<std::uint32_t>(trading_day);
    config.source_id = 88U;
    config.segment_bytes = segment_bytes;
    config.max_payload_bytes = static_cast<std::uint32_t>(max_payload);
    sze_recovery::JournalReader reader;
    const sze_recovery::JournalOpenResult opened = reader.open(config);
    if (opened.status != sze_recovery::kJournalOk) {
        std::cerr << "sze_recovery_verify open_status="
                  << static_cast<int>(opened.status) << std::endl;
        return 3;
    }

    std::vector<unsigned char> payload(config.max_payload_bytes);
    std::uint64_t events = 0U;
    std::uint64_t orders = 0U;
    std::uint64_t executions = 0U;
    for (;;) {
        sze_recovery::CanonicalEvent event;
        const sze_recovery::JournalStatus read_status = reader.next(
            &event, payload.data(), payload.size());
        if (read_status == sze_recovery::kJournalEnd ||
            read_status == sze_recovery::kJournalWouldBlock) {
            break;
        }
        if (read_status != sze_recovery::kJournalOk) {
            std::cerr << "sze_recovery_verify read_status="
                      << static_cast<int>(read_status)
                      << " event_id=" << reader.next_event_id() << std::endl;
            return 4;
        }

        LFL2OrderField live_order;
        LFL2OrderField replay_order;
        LFL2TradeField live_trade;
        LFL2TradeField replay_trade;
        std::memset(&live_order, 0, sizeof(live_order));
        std::memset(&replay_order, 0, sizeof(replay_order));
        std::memset(&live_trade, 0, sizeof(live_trade));
        std::memset(&replay_trade, 0, sizeof(replay_trade));
        const sze_md::DecodeStatus live = sze_md::decode_record(
            payload.data(), event.payload_size, &live_order, &live_trade);
        const sze_md::DecodeStatus replay = sze_md::decode_recovery_record(
            event, payload.data(), event.payload_size,
            &replay_order, &replay_trade);
        if (live != replay ||
            (live == sze_md::DecodeStatus::kOrder &&
             std::memcmp(&live_order, &replay_order, sizeof(live_order)) != 0) ||
            (live == sze_md::DecodeStatus::kExecution &&
             std::memcmp(&live_trade, &replay_trade, sizeof(live_trade)) != 0)) {
            std::cerr << "sze_recovery_verify normalized_mismatch=1"
                      << " event_id=" << event.event_id
                      << " live_status=" << static_cast<int>(live)
                      << " replay_status=" << static_cast<int>(replay)
                      << std::endl;
            return 5;
        }
        if (live == sze_md::DecodeStatus::kOrder) {
            ++orders;
        } else if (live == sze_md::DecodeStatus::kExecution) {
            ++executions;
        } else {
            std::cerr << "sze_recovery_verify unexpected_record=1"
                      << " event_id=" << event.event_id << std::endl;
            return 6;
        }
        ++events;
    }
    std::cout << "sze_recovery_verify status=PASS"
              << " events=" << events
              << " orders=" << orders
              << " executions=" << executions
              << " continuity=" << static_cast<int>(opened.continuity_state)
              << " invalid_reason=" << static_cast<int>(opened.invalid_reason)
              << " invalid_reason_name="
              << sze_recovery::invalid_reason_name(opened.invalid_reason)
              << " clean_shutdown=" << (opened.unclean_restart ? 0 : 1)
              << std::endl;
    return 0;
}
