#include "sse/runtime/sse_stream_processor.h"
#include "common/stream/MarketDataStream.h"
#include "common/stream/UdpChannelRuntime.h"
#include "tests/sse/sse_test_artifacts.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void put_u32(std::vector<unsigned char>* bytes, std::size_t offset,
             std::uint32_t value) {
    for (std::size_t i = 0; i < 4U; ++i)
        (*bytes)[offset + i] = static_cast<unsigned char>((value >> (8U * i)) & 0xffU);
}

void put_u64(std::vector<unsigned char>* bytes, std::size_t offset,
             std::uint64_t value) {
    for (std::size_t i = 0; i < 8U; ++i)
        (*bytes)[offset + i] = static_cast<unsigned char>((value >> (8U * i)) & 0xffU);
}

void put_ascii(std::vector<unsigned char>* bytes, std::size_t offset,
               const std::string& value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
        (*bytes)[offset + i] = i < value.size()
            ? static_cast<unsigned char>(value[i]) : static_cast<unsigned char>(' ');
}

std::vector<unsigned char> tick(std::uint32_t sequence, std::uint64_t index,
                                char type, char side, std::uint32_t time_raw,
                                std::uint64_t buy_order, std::uint64_t sell_order,
                                std::uint64_t quantity = 1000000ULL,
                                const std::string& security = "600000") {
    std::vector<unsigned char> bytes(72U, 0U);
    bytes[8] = 0x3eU;
    put_u32(&bytes, 0U, sequence);
    put_u64(&bytes, 9U, index);
    bytes[17] = 7U;  // wire channel number
    bytes[18] = 0U;
    put_ascii(&bytes, 21U, security, 8U);
    put_u32(&bytes, 30U, time_raw);
    bytes[34] = static_cast<unsigned char>(type);
    put_u64(&bytes, 35U, buy_order);
    put_u64(&bytes, 43U, sell_order);
    put_u32(&bytes, 51U, 10000U);
    put_u64(&bytes, 55U, quantity);
    put_u64(&bytes, 63U, 0U);
    bytes[71] = static_cast<unsigned char>(side);
    return bytes;
}

std::vector<unsigned char> primary_heartbeat() {
    std::vector<unsigned char> bytes(32U, 0U);
    put_u32(&bytes, 0U, 999U);
    bytes[8] = 0xa2U;
    std::memcpy(&bytes[16], &bytes[0], 16U);
    return bytes;
}

std::vector<unsigned char> snapshot(std::uint32_t sequence,
                                    std::uint32_t time_raw,
                                    std::uint64_t volume,
                                    std::uint64_t turnover,
                                    const std::string& security = "600000") {
    std::vector<unsigned char> bytes(440U, 0U);
    bytes[8] = 0x27U;
    put_u32(&bytes, 0U, sequence);
    put_u32(&bytes, 21U, sequence);
    put_ascii(&bytes, 30U, security, 8U);
    put_u32(&bytes, 26U, time_raw);
    put_u32(&bytes, 42U, 10000U);
    put_u32(&bytes, 46U, 10000U);
    put_u32(&bytes, 50U, 11000U);
    put_u32(&bytes, 54U, 9000U);
    put_u32(&bytes, 58U, 10500U);
    put_u64(&bytes, 74U, volume * 1000ULL);
    put_u64(&bytes, 82U, turnover * 100000ULL);
    for (std::size_t level = 0; level < 5U; ++level) {
        const std::size_t bid = 124U + level * 16U;
        const std::size_t ask = 124U + (10U + level) * 16U;
        put_u32(&bytes, bid, 10000U - static_cast<std::uint32_t>(level) * 10U);
        put_u64(&bytes, bid + 4U, 1000000ULL);
        put_u32(&bytes, ask, 11000U + static_cast<std::uint32_t>(level) * 10U);
        put_u64(&bytes, ask + 4U, 1000000ULL);
    }
    return bytes;
}

deepwin_market_data::StreamEvent event(const std::vector<unsigned char>& bytes,
                                       std::uint64_t sequence,
                                       std::uint64_t monotonic_ns,
                                       std::uint64_t receive_batch = 3U,
                                       std::uint32_t batch_index = 0U,
                                       std::uint32_t batch_size = 1U) {
    deepwin_market_data::StreamEvent value = {};
    value.kind = deepwin_market_data::kDatagramEvent;
    value.sequence = sequence;
    value.monotonic_ns = monotonic_ns;
    value.realtime_ns = 100000000000ULL + monotonic_ns;
    value.receive_batch = receive_batch;
    value.channel_id = 4U;
    value.batch_index = batch_index;
    value.batch_size = batch_size;
    value.source_port = 37109U;
    value.data = bytes.empty() ? 0 : &bytes[0];
    value.size = bytes.size();
    return value;
}

deepwin_market_data::StreamEvent idle(std::uint64_t sequence,
                                      std::uint64_t monotonic_ns) {
    deepwin_market_data::StreamEvent value = {};
    value.kind = deepwin_market_data::kIdleEvent;
    value.sequence = sequence;
    value.monotonic_ns = monotonic_ns;
    value.realtime_ns = 100000000000ULL + monotonic_ns;
    value.channel_id = 4U;
    return value;
}

deepwin_market_data::StreamEvent hardware_event(
    const std::vector<unsigned char>& bytes, std::uint64_t sequence,
    std::uint64_t monotonic_ns, std::uint64_t hardware_ns,
    std::uint32_t channel_id = 4U) {
    deepwin_market_data::StreamEvent value = event(
        bytes, sequence, monotonic_ns, sequence, 0U, 1U);
    value.channel_id = channel_id;
    value.timestamp_flags = static_cast<std::uint16_t>(
        deepwin_market_data::kKernelRealtimeTimestamp |
        deepwin_market_data::kHardwareReceiveTimestamp |
        deepwin_market_data::kHardwareTimestampRequested);
    value.hardware_ns = hardware_ns;
    value.application_realtime_ns = value.realtime_ns;
    value.hardware_clock_index = 0;
    return value;
}

sse_tick::DailyStaticMetadataMap metadata() {
    sse_tick::DailyStaticMetadata value;
    value.date = 20260818U;
    value.avg_amount = 100000000.0;
    value.turnover_threshold = 0.1;
    value.free_share = 1000000.0;
    value.pre_close = 10.0;
    value.limit_price = 11.0;
    value.stop_price = 9.0;
    value.has_date = value.has_avg_amount = value.has_turnover_threshold = true;
    value.has_free_share = value.has_pre_close = value.has_limit_price = true;
    value.has_stop_price = true;
    value.quality = "test";
    sse_tick::DailyStaticMetadataMap result;
    result["600000"] = value;
    return result;
}

sse_tick::DailyStaticMetadataMap metadata_two_instruments() {
    sse_tick::DailyStaticMetadataMap result = metadata();
    result["600001"] = result["600000"];
    return result;
}

void assert_a_book(const sse_stream::TickOutput& output) {
    assert(output.event.security_id == "600000");
    assert(output.bid_levels.size() == 10U && output.ask_levels.size() == 10U);
    assert(output.bid_levels[0].price_raw == 10000);
    assert(output.ask_levels[0].price_raw == 10000);
}

void test_tick_batch_and_provenance() {
    std::vector<sse_stream::Output> outputs;
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true,
        [&outputs](const sse_stream::Output& output) { outputs.push_back(output); });
    std::vector<unsigned char> first = tick(1U, 1U, 'A', 0, 9300000U, 1001U, 0U);
    std::vector<unsigned char> second = tick(2U, 2U, 'A', 1, 9300000U, 0U, 2001U);
    std::vector<unsigned char> burst(first);
    burst.insert(burst.end(), second.begin(), second.end());
    const std::vector<unsigned char> opening_trade = tick(
        3U, 3U, 'T', 0, 9300100U, 1001U, 2001U, 100000U);
    burst.insert(burst.end(), opening_trade.begin(), opening_trade.end());
    processor.on_event(event(burst, 10U, 1000U, 9U, 0U, 2U));
    assert(outputs.empty());  // receive batch/datagram end is not a sample boundary
    processor.on_event(idle(11U, 101001U));
    assert(outputs.size() == 1U);
    assert(outputs[0].kind == sse_stream::kTickOutput);
    assert(outputs[0].tick.event.tick_index == 3U);

    std::vector<unsigned char> trade = tick(4U, 4U, 'T', 0, 9300200U,
                                             1001U, 2001U, 100000U);
    std::vector<unsigned char> trade_and_next = trade;
    const std::vector<unsigned char> next_order = tick(
        5U, 5U, 'A', 0, 9300200U, 1002U, 0U, 100000U);
    trade_and_next.insert(trade_and_next.end(), next_order.begin(), next_order.end());
    processor.on_event(event(trade_and_next, 12U, 200000U, 3U, 0U, 2U));
    processor.on_event(idle(13U, 301001U));
    assert(outputs.size() == 2U);
    const sse_stream::Output& output = outputs[1];
    assert(output.kind == sse_stream::kTickOutput);
    assert(output.tick.event.channel_no == 7U);
    assert(output.tick.event.tick_index == 5U);
    assert(output.tick.provenance.wire_channel_no == 7U);
    assert(output.tick.provenance.wire_sequence == 5U);
    assert(output.tick.provenance.record_offset == 72U);
    assert(output.tick.provenance.stream_sequence == 12U);
    assert(output.tick.provenance.receive_batch == 3U);
    assert(output.tick.provenance.batch_id != 0U);
    assert(output.tick.bid_levels.size() == 10U);
    assert(output.tick.ask_levels.size() == 10U);
    assert(output.tick.factors.values.size() == 50U);
}

void test_hardware_batch_end_marker() {
    std::vector<sse_stream::Output> outputs;
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true,
        [&outputs](const sse_stream::Output& output) { outputs.push_back(output); });
    std::vector<unsigned char> opening = tick(1U, 1U, 'A', 0, 9300000U, 1001U, 0U);
    const std::vector<unsigned char> ask = tick(2U, 2U, 'A', 1, 9300000U, 0U, 2001U);
    opening.insert(opening.end(), ask.begin(), ask.end());
    processor.on_event(hardware_event(opening, 1U, 1000U, 1000000ULL));
    processor.on_event(hardware_event(
        tick(3U, 3U, 'T', 0, 9300100U, 1001U, 2001U, 100000U),
        2U, 2000U, 1001200ULL));
    assert(outputs.empty());

    processor.on_event(hardware_event(
        tick(4U, 4U, 'T', 0, 9300200U, 1001U, 2001U, 100000U),
        3U, 3000U, 1010000ULL));
    assert(outputs.size() == 2U);
    assert(outputs[0].kind == sse_stream::kTickOutput);
    assert(outputs[1].kind == sse_stream::kBatchEndOutput);
    assert(outputs[1].batch_end.packet_count == 2U);
    assert(outputs[1].batch_end.candidate_count == 1U);
    assert(outputs[1].batch_end.prediction_count == 0U);
    assert(outputs[0].tick.provenance.batch_id == outputs[1].batch_end.batch_id);

    processor.on_event(idle(4U, 100000U));
    assert(outputs.size() == 4U);
    assert(outputs[3].kind == sse_stream::kBatchEndOutput);
    assert(outputs[2].tick.provenance.batch_id == outputs[3].batch_end.batch_id);
}

void test_hardware_batches_are_per_channel() {
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true, [](const sse_stream::Output&) {});
    std::vector<unsigned char> opening = tick(1U, 1U, 'A', 0, 9300000U, 1001U, 0U);
    const std::vector<unsigned char> ask = tick(2U, 2U, 'A', 1, 9300000U, 0U, 2001U);
    opening.insert(opening.end(), ask.begin(), ask.end());
    processor.on_event(hardware_event(opening, 1U, 1000U, 1000000ULL, 0U));
    processor.on_event(hardware_event(
        snapshot(1U, 93000U, 1000U, 1000U), 2U, 2000U, 900000ULL, 1U));
    processor.on_event(hardware_event(
        tick(3U, 3U, 'T', 0, 9300100U, 1001U, 2001U, 100000U),
        3U, 3000U, 1001200ULL, 0U));
    assert(!processor.invalid());
    processor.on_event(idle(4U, 100000U));
    assert(!processor.invalid());
}

void test_hardware_timestamp_regression_is_per_channel() {
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true, [](const sse_stream::Output&) {});
    const std::vector<unsigned char> first = tick(
        1U, 1U, 'A', 0, 9300000U, 1001U, 0U);
    processor.on_event(hardware_event(first, 1U, 1000U, 1000000ULL, 2U));
    processor.on_event(idle(2U, 2000U));
    bool threw = false;
    try {
        processor.on_event(hardware_event(
            tick(2U, 2U, 'A', 1, 9300000U, 0U, 2001U),
            3U, 3000U, 999999ULL, 2U));
    } catch (const std::runtime_error&) { threw = true; }
    assert(!threw && !processor.invalid());
}

void test_duplicate_and_gap_are_distinct() {
    std::vector<sse_stream::Output> outputs;
    sse_stream::SseStreamProcessor duplicate_processor(
        metadata(), 0, true,
        [&outputs](const sse_stream::Output& output) { outputs.push_back(output); });
    std::vector<unsigned char> one = tick(1U, 1U, 'A', 0, 9300000U, 1001U, 0U);
    duplicate_processor.on_event(event(one, 1U, 1000U));
    duplicate_processor.on_event(event(one, 2U, 1000U));
    assert(!duplicate_processor.invalid());

    sse_stream::SseStreamProcessor gap_processor(
        metadata(), 0, true,
        [](const sse_stream::Output&) {});
    std::vector<unsigned char> gap = tick(3U, 3U, 'A', 0, 9300000U, 1001U, 0U);
    gap_processor.on_event(event(one, 1U, 1000U));
    bool threw = false;
    try { gap_processor.on_event(event(gap, 2U, 2000U)); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw && gap_processor.invalid());
    threw = false;
    try { gap_processor.on_event(event(one, 3U, 3000U)); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

void test_unconfigured_market_records_count_for_sequence_only() {
    std::vector<sse_stream::Output> outputs;
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true,
        [&outputs](const sse_stream::Output& output) { outputs.push_back(output); });
    const std::vector<unsigned char> unknown_equity = tick(
        1U, 1U, 'A', 0, 9300000U, 1001U, 0U, 1000000ULL, "600001");
    const std::vector<unsigned char> etf = tick(
        2U, 2U, 'A', 0, 9300000U, 1001U, 0U, 1000000ULL, "510050");
    const std::vector<unsigned char> target_bid = tick(
        3U, 3U, 'A', 0, 9300000U, 1001U, 0U);
    const std::vector<unsigned char> target_ask = tick(
        4U, 4U, 'A', 1, 9300000U, 0U, 2001U);
    processor.on_event(event(unknown_equity, 1U, 1000U));
    processor.on_event(event(etf, 2U, 2000U));
    processor.on_event(event(target_bid, 3U, 3000U));
    processor.on_event(event(target_ask, 4U, 4000U));
    processor.on_event(idle(5U, 105000U));
    assert(!processor.invalid());
    assert(outputs.empty());  // target's first valid book cut only seeds its gate
}

void test_per_instrument_quiet_stock_closes_while_other_updates() {
    std::vector<sse_stream::Output> outputs;
    sse_stream::SseStreamProcessor processor(
        metadata_two_instruments(), 0, true,
        [&outputs](const sse_stream::Output& output) { outputs.push_back(output); });
    std::vector<unsigned char> a = tick(
        1U, 1U, 'A', 0, 9300000U, 1001U, 0U, 1000000ULL, "600000");
    const std::vector<unsigned char> a_ask = tick(
        2U, 2U, 'A', 1, 9300000U, 0U, 2001U, 1000000ULL, "600000");
    a.insert(a.end(), a_ask.begin(), a_ask.end());
    const std::vector<unsigned char> a_trade = tick(
        3U, 3U, 'T', 0, 9300100U, 1001U, 2001U, 100000U, "600000");
    a.insert(a.end(), a_trade.begin(), a_trade.end());
    processor.on_event(event(a, 1U, 1000U));

    const std::vector<unsigned char> b_bid = tick(
        4U, 4U, 'A', 0, 9300000U, 3001U, 0U, 1000000ULL, "600001");
    const std::vector<unsigned char> b_ask = tick(
        5U, 5U, 'A', 1, 9300000U, 0U, 4001U, 1000000ULL, "600001");
    const std::vector<unsigned char> b_trade = tick(
        6U, 6U, 'T', 0, 9300100U, 3001U, 4001U, 1U, "600001");
    processor.on_event(event(b_bid, 2U, 50000U));
    processor.on_event(event(b_ask, 3U, 50000U));
    processor.on_event(event(b_trade, 4U, 90000U));
    // A is due at 101001ns. B remains active because its latest update was
    // at 90000ns; the processor must emit A before applying this B update.
    const std::vector<unsigned char> b_later = tick(
        7U, 7U, 'A', 0, 9300100U, 3002U, 0U, 1U, "600001");
    processor.on_event(event(b_later, 5U, 101001U));

    assert(!processor.invalid());
    assert(outputs.size() == 1U);
    assert(outputs[0].kind == sse_stream::kTickOutput);
    assert(outputs[0].tick.event.tick_index == 3U);
    assert(outputs[0].tick.provenance.wire_sequence == 3U);
    assert(outputs[0].tick.provenance.batch_emitted_ns == 101001U);
    assert_a_book(outputs[0].tick);
}

void test_same_exchange_time_duplicate_bursts_accumulate() {
    std::vector<sse_stream::Output> outputs;
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true,
        [&outputs](const sse_stream::Output& output) { outputs.push_back(output); });
    const std::vector<unsigned char> opening_bid = tick(
        1U, 1U, 'A', 0, 9300000U, 1001U, 0U);
    const std::vector<unsigned char> opening_ask = tick(
        2U, 2U, 'A', 1, 9300000U, 0U, 2001U);
    processor.on_event(event(opening_bid, 1U, 1000U));
    processor.on_event(event(opening_ask, 2U, 1000U));

    // The first distinct exchange time accepts an amount-triggered sample.
    processor.on_event(event(tick(3U, 3U, 'T', 0, 9300100U,
                                  1001U, 2001U, 100000U), 3U, 1000U));
    processor.on_event(idle(4U, 101001U));
    assert(outputs.size() == 1U && outputs[0].tick.event.tick_index == 3U);

    // A second burst at the same exchange timestamp is deliberately ignored
    // by the gate, even though its amount independently exceeds threshold.
    processor.on_event(event(tick(4U, 4U, 'T', 0, 9300100U,
                                  1001U, 2001U, 100000U), 5U, 200000U));
    processor.on_event(idle(6U, 301001U));
    assert(outputs.size() == 1U);

    // This amount is below the threshold alone, but the duplicate-time flow
    // plus this next-time flow is visible in the accepted sample.
    processor.on_event(event(tick(5U, 5U, 'T', 0, 9300200U,
                                  1001U, 2001U, 1U), 7U, 400000U));
    processor.on_event(idle(8U, 501001U));
    assert(outputs.size() == 2U);
    assert(outputs[1].tick.event.tick_index == 5U);
    assert(outputs[1].tick.sample_decision.accepted);
    assert((outputs[1].tick.sample_decision.reasons &
            sse_live_sampling::kTurnoverSampleReason) != 0);
}

void test_preopen_does_not_seed_tick_window() {
    std::vector<sse_stream::Output> outputs;
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true,
        [&outputs](const sse_stream::Output& output) { outputs.push_back(output); });
    processor.on_event(event(tick(1U, 1U, 'A', 0, 9295900U,
                                  1001U, 0U), 1U, 1000U));
    processor.on_event(event(tick(2U, 2U, 'A', 1, 9295900U,
                                  0U, 2001U), 2U, 1000U));
    processor.on_event(event(tick(3U, 3U, 'T', 0, 9295900U,
                                  1001U, 2001U, 100000U), 3U, 1000U));
    processor.on_event(idle(4U, 101001U));
    assert(outputs.empty());

    // The first post-open two-sided update initializes from 09:30, despite
    // the large pre-open turnover already present in the reconstructed book.
    processor.on_event(event(tick(4U, 4U, 'A', 0, 9300000U,
                                  1002U, 0U), 5U, 200000U));
    processor.on_event(event(tick(5U, 5U, 'T', 0, 9300100U,
                                  1002U, 2001U, 10U), 6U, 400000U));
    processor.on_event(idle(7U, 501001U));
    assert(outputs.size() == 1U);
    assert(outputs[0].tick.event.tick_index == 5U);
    assert(outputs[0].tick.sample_decision.accepted);
    assert(std::fabs(outputs[0].tick.sample_decision.window_turnover - 0.1) < 1.0e-9);
}

void test_snapshot36_and_parity() {
    std::vector<sse_stream::Output> live_outputs;
    std::vector<sse_stream::Output> replay_outputs;
    std::vector<unsigned char> first = snapshot(1U, 93000U, 1000U, 1000U);
    std::vector<unsigned char> second = snapshot(2U, 93100U, 1100U, 1100U);
    sse_stream::SseStreamProcessor live(
        metadata(), 0, true,
        [&live_outputs](const sse_stream::Output& output) { live_outputs.push_back(output); });
    sse_stream::SseStreamProcessor replay(
        metadata(), 0, true,
        [&replay_outputs](const sse_stream::Output& output) { replay_outputs.push_back(output); });
    live.on_event(event(first, 20U, 5000U, 1U));
    live.on_event(event(second, 21U, 7000U, 2U));
    replay.on_event(event(first, 120U, 5000U, 1U));
    replay.on_event(event(second, 121U, 7000U, 2U));
    assert(live_outputs.size() == 1U && replay_outputs.size() == 1U);
    assert(live_outputs[0].kind == sse_stream::kSnapshotOutput);
    assert(live_outputs[0].snapshot.snapshot36.size() == 36U);
    assert(live_outputs[0].snapshot.prediction_valid == false);
    assert(live_outputs[0].snapshot.provenance.wire_sequence == 2U);
    assert(live_outputs[0].snapshot.snapshot.security_id ==
           replay_outputs[0].snapshot.snapshot.security_id);
    assert(live_outputs[0].snapshot.snapshot36 == replay_outputs[0].snapshot.snapshot36);
}

void test_snapshot_boundaries_and_regression() {
    std::vector<sse_stream::Output> outputs;
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true,
        [&outputs](const sse_stream::Output& output) { outputs.push_back(output); });
    std::vector<unsigned char> one_sided = snapshot(1U, 93000U, 1000U, 1000U);
    const std::size_t ask_level_one = 124U + 10U * 16U;
    put_u64(&one_sided, ask_level_one + 4U, 0U);
    processor.on_event(event(one_sided, 1U, 1000U));
    assert(outputs.empty());

    processor.on_event(event(snapshot(2U, 92959U, 1000U, 1000U), 2U, 2000U));
    processor.on_event(event(snapshot(3U, 93000U, 1100U, 1100U), 3U, 3000U));
    assert(outputs.size() == 1U && outputs[0].snapshot.snapshot36.size() == 36U);

    sse_stream::SseStreamProcessor regression(
        metadata(), 0, true, [](const sse_stream::Output&) {});
    regression.on_event(event(snapshot(4U, 93000U, 1000U, 1000U), 4U, 4000U));
    bool threw = false;
    try {
        regression.on_event(event(snapshot(5U, 93100U, 900U, 1100U), 5U, 5000U));
    } catch (const std::runtime_error&) { threw = true; }
    assert(threw && regression.invalid());
}

void test_callback_exception_is_sticky() {
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true,
        [](const sse_stream::Output&) { throw std::runtime_error("callback failure"); });
    processor.on_event(event(snapshot(1U, 93000U, 1000U, 1000U), 1U, 1000U));
    bool threw = false;
    try {
        processor.on_event(event(snapshot(2U, 93100U, 1100U, 1100U), 2U, 2000U));
    } catch (const std::runtime_error&) { threw = true; }
    assert(threw && processor.invalid());
}

void test_static_metadata_validation() {
    sse_tick::DailyStaticMetadataMap bad = metadata();
    bad["600001"] = bad["600000"];
    bad["600001"].date = 20260819U;
    bool threw = false;
    try {
        sse_stream::SseStreamProcessor processor(
            bad, 0, true, [](const sse_stream::Output&) {});
    } catch (const std::runtime_error&) { threw = true; }
    assert(threw);
    bad = metadata();
    bad["600000"].avg_amount = std::numeric_limits<double>::quiet_NaN();
    threw = false;
    try {
        sse_stream::SseStreamProcessor processor(
            bad, 0, true, [](const sse_stream::Output&) {});
    } catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

void test_malformed_is_sticky() {
    sse_stream::SseStreamProcessor processor(
        metadata(), 0, true, [](const sse_stream::Output&) {});
    std::vector<unsigned char> malformed(71U, 0U);
    bool threw = false;
    try { processor.on_event(event(malformed, 1U, 1U)); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw && processor.invalid() && !processor.invalid_reason().empty());
    threw = false;
    try { processor.on_event(idle(2U, 2U)); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

void test_primary_heartbeat() {
    sse_stream::SseStreamProcessor accepted(
        metadata(), 0, true, [](const sse_stream::Output&) {});
    const std::vector<unsigned char> pulse = primary_heartbeat();
    accepted.on_event(event(pulse, 1U, 1U));
    assert(!accepted.invalid());
    std::vector<unsigned char> snapshot_pulse = pulse;
    snapshot_pulse[8] = snapshot_pulse[24] = 0x8bU;
    accepted.on_event(event(snapshot_pulse, 2U, 2U));
    assert(!accepted.invalid());
    snapshot_pulse[24] = 0xa2U;
    assert(!sse_live::is_primary_heartbeat(snapshot_pulse.data(), snapshot_pulse.size()));
    snapshot_pulse[8] = snapshot_pulse[24] = 0x8cU;
    assert(!sse_live::is_primary_heartbeat(snapshot_pulse.data(), snapshot_pulse.size()));

    sse_stream::SseStreamProcessor mismatched(
        metadata(), 0, true, [](const sse_stream::Output&) {});
    std::vector<unsigned char> corrupt = pulse;
    corrupt[31] = 1U;
    bool threw = false;
    try { mismatched.on_event(event(corrupt, 1U, 1U)); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw && mismatched.invalid());

    sse_stream::SseStreamProcessor reserved(
        metadata(), 0, true, [](const sse_stream::Output&) {});
    corrupt = pulse;
    corrupt[4] = corrupt[20] = 1U;
    threw = false;
    try { reserved.on_event(event(corrupt, 1U, 1U)); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw && reserved.invalid());
}

std::vector<std::vector<unsigned char> > auction_packets() {
    struct Row {
        char type, side;
        std::uint32_t time;
        std::uint64_t buy, sell, shares;
        std::uint32_t price;
    };
    // Conserved auction with a varying clearing-price path and both sides of
    // the last-minute book. No fabricated zero/NaN replacement is needed.
    const Row rows[] = {
        {'A', 0, 9150000U, 1, 0, 80, 10000},
        {'A', 1, 9150000U, 0, 2, 70, 9980},
        {'A', 1, 9170000U, 0, 4, 90, 9990},
        {'A', 0, 9180000U, 5, 0, 50, 10000},
        {'A', 1, 9180000U, 0, 6, 60, 9990},
        {'D', 0, 9180400U, 5, 0, 50, 10000},
        {'D', 1, 9180400U, 0, 6, 60, 9990},
        {'A', 1, 9190000U, 0, 7, 80, 10000},
        {'A', 1, 9200000U, 0, 8, 40, 10010},
        {'A', 0, 9210000U, 9, 0, 50, 10000},
        {'A', 0, 9240000U, 3, 0, 120, 10020},
        {'A', 0, 9240000U, 10, 0, 30, 9990},
        {'T', 0, 9250000U, 1, 2, 70, 10000},
        {'T', 0, 9250000U, 1, 4, 10, 10000},
        {'T', 0, 9250000U, 3, 4, 80, 10000},
        {'T', 0, 9250000U, 3, 7, 40, 10000},
        {'T', 0, 9250000U, 9, 7, 40, 10000},
        {'S', 3, 9250000U, 0, 0, 0, 0}
    };
    std::vector<std::vector<unsigned char> > result;
    for (std::size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const Row& row = rows[i];
        std::vector<unsigned char> bytes = tick(
            static_cast<std::uint32_t>(i + 1U), i + 1U, row.type, row.side,
            row.time, row.buy, row.sell, row.shares * 1000ULL);
        put_u32(&bytes, 51U, row.price);
        result.push_back(bytes);
    }
    return result;
}

void test_inline_auction_model(const sse_hybrid_model::Model& model) {
    const std::vector<std::vector<unsigned char> > packets = auction_packets();
    std::vector<float> reference_factors;
    for (int mode = 0; mode < 4; ++mode) {
        std::vector<sse_stream::Output> outputs;
        sse_stream::SseStreamProcessor processor(
            metadata_two_instruments(), &model, false,
            [&outputs](const sse_stream::Output& output) { outputs.push_back(output); });
        std::uint64_t arrival = 0;
        const auto feed = [&processor, &arrival](const std::vector<unsigned char>& bytes) {
            ++arrival;
            processor.on_event(event(bytes, arrival, arrival * 10000ULL));
        };
        // Modes cover snapshot before S, S before snapshot, first available
        // snapshot at 09:30, and missing S. The second stock has no auction.
        if (mode == 0) feed(snapshot(1U, 92501U, 240U, 2400U));
        for (std::size_t i = 0; i < packets.size() - (mode == 3 ? 1U : 0U); ++i)
            feed(packets[i]);
        if (mode == 1 || mode == 3) feed(snapshot(2U, 92501U, 240U, 2400U));
        feed(snapshot(3U, 92501U, 240U, 2400U, "600001"));
        feed(snapshot(4U, 93000U, 1000U, 10000U, "600001"));
        // A continuous-session total includes post-auction trades. It must
        // not be compared for equality against the auction's 240 shares.
        feed(snapshot(5U, 93000U, 1000U, 10000U));
        feed(snapshot(6U, 93001U, 1100U, 11000U));
        assert(!processor.invalid());
        if (mode == 3) {
            assert(outputs.empty());
            // Once opening inference has been skipped, late auction state
            // must not start a partially initialized Snapshot model mid-day.
            feed(packets.back());
            feed(snapshot(7U, 93002U, 1200U, 12000U));
            assert(outputs.empty());
            continue;
        }
        assert(!outputs.empty());
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            assert(outputs[i].kind == sse_stream::kSnapshotOutput);
            assert(outputs[i].snapshot.snapshot.security_id == "600000");
            assert(outputs[i].snapshot.prediction_valid);
            assert(outputs[i].snapshot.prediction.selected);
            assert(outputs[i].snapshot.auction59.size() == 59U);
            if (reference_factors.empty()) reference_factors = outputs[i].snapshot.auction59;
            assert(outputs[i].snapshot.auction59 == reference_factors);
        }
    }
}

void test_synthetic_model_wiring() {
    const std::string prefix = std::string("/tmp/sse_stream_processor_model_") +
                               std::to_string(static_cast<long long>(::getpid()));
    const std::string tick_path = prefix + ".ssemodl1";
    const std::string baseline_path = prefix + ".base.ssegru";
    const std::string auction_path = prefix + ".auction.ssegru";
    const std::string baseline_scaler = prefix + ".base.json";
    const std::string auction_scaler = prefix + ".auction.json";
    sse_test_artifacts::write_tick_artifact(tick_path);
    sse_test_artifacts::write_snapshot_artifact(baseline_path, 36U);
    sse_test_artifacts::write_snapshot_artifact(auction_path, 95U);
    sse_test_artifacts::write_scaler(baseline_scaler, 36U);
    sse_test_artifacts::write_scaler(auction_scaler, 95U);

    sse_hybrid_model::Model model;
    std::string error;
    assert(model.load(tick_path, baseline_path, baseline_scaler,
                      auction_path, auction_scaler, &error));
    test_inline_auction_model(model);
    std::vector<sse_stream::Output> outputs;
    sse_stream::Auction59Provider provider =
        [](const std::string&, std::uint64_t, std::vector<float>* values,
           std::string*) {
            values->assign(59U, 0.0f);
            return true;
        };
    sse_stream::SseStreamProcessor processor(
        metadata(), &model, false,
        [&outputs](const sse_stream::Output& output) { outputs.push_back(output); },
        provider);
    const std::vector<unsigned char> snap1 = snapshot(10U, 93000U, 1000U, 1000U);
    const std::vector<unsigned char> snap2 = snapshot(11U, 93100U, 1100U, 1100U);
    processor.on_event(event(snap1, 1U, 100U));
    processor.on_event(event(snap2, 2U, 200U));
    assert(outputs.size() == 1U);
    assert(outputs.back().kind == sse_stream::kSnapshotOutput);
    assert(outputs.back().snapshot.prediction_valid);
    assert(outputs.back().snapshot.prediction.selected);
    assert(outputs.back().snapshot.auction59.size() == 59U);
    outputs.clear();
    std::vector<unsigned char> opening = tick(1U, 1U, 'A', 0, 9350000U, 1001U, 0U);
    const std::vector<unsigned char> ask = tick(2U, 2U, 'A', 1, 9350000U, 0U, 2001U);
    opening.insert(opening.end(), ask.begin(), ask.end());
    const std::vector<unsigned char> trade = tick(
        3U, 3U, 'T', 0, 9350000U, 1001U, 2001U, 100000U);
    opening.insert(opening.end(), trade.begin(), trade.end());
    processor.on_event(event(opening, 3U, 1000U));
    processor.on_event(idle(4U, 101001U));
    processor.on_event(event(tick(4U, 4U, 'T', 0, 9350100U,
                              1001U, 2001U, 100000U), 5U, 200000U));
    processor.on_event(idle(6U, 301001U));
    const std::vector<unsigned char> later = tick(
        5U, 5U, 'T', 0, 9350200U, 1001U, 2001U, 100000U);
    processor.on_event(event(later, 7U, 400000U));
    processor.on_event(idle(8U, 501001U));
    assert(outputs.size() >= 2U);
    assert(outputs[0].kind == sse_stream::kTickOutput);
    assert(outputs[0].tick.prediction_valid);
    assert(!outputs[0].tick.prediction.selected);  // incomplete seeded row
    assert(outputs[1].tick.prediction_valid);
    assert(outputs[1].tick.prediction.selected);
    assert(outputs[1].tick.prediction.selected_source == sse_hybrid_model::kTickSource);

    // Missing auction history no longer prevents starting the model stream.
    // Its tick state warms before 09:35, while only its own Snapshot is skipped.
    std::vector<sse_stream::Output> fallback_outputs;
    sse_stream::SseStreamProcessor missing_auction(
        metadata(), &model, false,
        [&fallback_outputs](const sse_stream::Output& output) {
            fallback_outputs.push_back(output);
        });
    assert(missing_auction.instrument_static_valid("600000"));
    assert(!missing_auction.instrument_static_valid("600001"));
    missing_auction.on_event(event(snapshot(1U, 92500U, 1000U, 10000U), 1U, 1000U));
    missing_auction.on_event(event(snapshot(2U, 93000U, 1100U, 11000U), 2U, 2000U));
    missing_auction.on_event(event(snapshot(3U, 93100U, 1200U, 12000U), 3U, 3000U));
    // A broken/stale Snapshot cumulative total is irrelevant after this stock
    // has locked tick-only; it must not throw and abort all tick processing.
    missing_auction.on_event(event(snapshot(4U, 93200U, 100U, 1000U), 4U, 4000U));
    assert(fallback_outputs.empty() && !missing_auction.invalid());
    std::vector<unsigned char> fallback_opening = tick(1U, 1U, 'A', 0, 9300000U, 1001U, 0U);
    const std::vector<unsigned char> fallback_ask = tick(2U, 2U, 'A', 1, 9300000U, 0U, 2001U);
    const std::vector<unsigned char> fallback_trade = tick(3U, 3U, 'T', 0, 9300100U,
                                                         1001U, 2001U, 100000U);
    fallback_opening.insert(fallback_opening.end(), fallback_ask.begin(), fallback_ask.end());
    fallback_opening.insert(fallback_opening.end(), fallback_trade.begin(), fallback_trade.end());
    missing_auction.on_event(event(fallback_opening, 4U, 10000U));
    missing_auction.on_event(idle(5U, 111001U));
    missing_auction.on_event(event(tick(4U, 4U, 'T', 0, 9345900U,
                                      1001U, 2001U, 100000U), 6U, 200000U));
    missing_auction.on_event(idle(7U, 301001U));
    missing_auction.on_event(event(tick(5U, 5U, 'T', 0, 9350000U,
                                      1001U, 2001U, 100000U), 8U, 400000U));
    missing_auction.on_event(idle(9U, 501001U));
    assert(fallback_outputs.size() == 3U);
    for (std::size_t i = 0; i < fallback_outputs.size(); ++i) {
        assert(fallback_outputs[i].kind == sse_stream::kTickOutput);
        assert(fallback_outputs[i].tick.prediction_valid);
    }
    assert(!fallback_outputs[0].tick.prediction.selected);
    assert(!fallback_outputs[1].tick.prediction.selected);
    assert(fallback_outputs[2].tick.prediction.selected);

    // Even a one-sided later snapshot must still close the common static gate.
    // Auction fallback must not let a contradictory daily pre-close trade.
    std::vector<unsigned char> conflict = snapshot(4U, 93501U, 1300U, 13000U);
    put_u32(&conflict, 42U, 10100U);
    put_u64(&conflict, 124U + 10U * 16U + 4U, 0U);
    missing_auction.on_event(event(conflict, 10U, 600000U));
    assert(!missing_auction.instrument_static_valid("600000"));
    missing_auction.on_event(event(tick(6U, 6U, 'T', 0, 9350200U,
                                      1001U, 2001U, 100000U), 11U, 700000U));
    missing_auction.on_event(idle(12U, 801001U));
    assert(fallback_outputs.back().tick.prediction_valid);
    assert(!fallback_outputs.back().tick.prediction.selected);
    assert(!missing_auction.invalid());

    std::vector<sse_stream::Output> no_snapshot_outputs;
    sse_stream::SseStreamProcessor no_snapshot(
        metadata(), &model, false,
        [&no_snapshot_outputs](const sse_stream::Output& output) { no_snapshot_outputs.push_back(output); },
        [](const std::string&, std::uint64_t, std::vector<float>*, std::string*) -> bool {
            assert(false); return false;
        });
    std::vector<unsigned char> no_snapshot_opening(opening);
    // The first book cut seeds the gate at 09:35:00. A trade at that exact
    // exchange timestamp intentionally does not sample; give this fixture's
    // first trade its own timestamp so two eligible windows are exercised.
    put_u32(&no_snapshot_opening, 2U * 72U + 30U, 9350001U);
    no_snapshot.on_event(event(no_snapshot_opening, 1U, 1000U));
    no_snapshot.on_event(idle(2U, 101001U));
    no_snapshot.on_event(event(tick(4U, 4U, 'T', 0, 9350100U,
                                   1001U, 2001U, 100000U), 3U, 200000U));
    no_snapshot.on_event(idle(4U, 301001U));
    assert(no_snapshot_outputs.size() == 2U);
    assert(no_snapshot_outputs[0].tick.event.tick_index == 3U);
    assert(no_snapshot_outputs[0].tick.prediction_valid);
    assert(!no_snapshot_outputs[0].tick.prediction.selected);
    assert(no_snapshot_outputs[1].tick.event.tick_index == 4U);
    assert(no_snapshot_outputs.back().tick.prediction.selected);
    no_snapshot.on_event(event(snapshot(1U, 93501U, 1000U, 10000U), 5U, 400000U));
    no_snapshot.on_event(event(snapshot(2U, 93502U, 1100U, 11000U), 6U, 500000U));
    assert(no_snapshot_outputs.size() == 2U && !no_snapshot.invalid());

    std::vector<sse_stream::Output> independent_outputs;
    sse_stream::SseStreamProcessor per_stock(
        metadata_two_instruments(), &model, false,
        [&independent_outputs](const sse_stream::Output& output) {
            independent_outputs.push_back(output);
        },
        [](const std::string& code, std::uint64_t, std::vector<float>* values,
           std::string* error) {
            if (code == "600001") { *error = "test_missing_history"; return false; }
            values->assign(59U, 0.0f);
            return true;
        });
    per_stock.on_event(event(snapshot(1U, 92500U, 240U, 2400U), 1U, 1000U));
    per_stock.on_event(event(snapshot(2U, 92500U, 240U, 2400U, "600001"), 2U, 2000U));
    per_stock.on_event(event(snapshot(3U, 93000U, 300U, 3000U, "600001"), 3U, 3000U));
    per_stock.on_event(event(snapshot(4U, 93000U, 300U, 3000U), 4U, 4000U));
    assert(!per_stock.invalid() && independent_outputs.size() == 1U);
    assert(independent_outputs[0].snapshot.snapshot.security_id == "600000");
    assert(independent_outputs[0].snapshot.prediction_valid);
    assert(independent_outputs[0].snapshot.prediction.selected);

    // Explicit tick-only configuration cannot accidentally invoke a provider.
    std::vector<sse_stream::Output> disabled_outputs;
    sse_stream::SseStreamProcessor disabled(
        metadata(), &model, false,
        [&disabled_outputs](const sse_stream::Output& output) { disabled_outputs.push_back(output); },
        [](const std::string&, std::uint64_t, std::vector<float>*, std::string*) -> bool {
            assert(false); return false;
        }, sse_auction59::StaticMetadataMap(), false);
    disabled.on_event(event(snapshot(1U, 92500U, 240U, 2400U), 1U, 1000U));
    disabled.on_event(event(snapshot(2U, 93000U, 300U, 3000U), 2U, 2000U));
    assert(disabled_outputs.empty() && !disabled.invalid());
    std::remove(tick_path.c_str());
    std::remove(baseline_path.c_str());
    std::remove(auction_path.c_str());
    std::remove(baseline_scaler.c_str());
    std::remove(auction_scaler.c_str());
}

void remove_recording(const std::string& directory) {
    DIR* dir = ::opendir(directory.c_str());
    if (!dir) return;
    while (dirent* entry = ::readdir(dir)) {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
        ::unlink((directory + "/" + entry->d_name).c_str());
    }
    ::closedir(dir);
    ::rmdir(directory.c_str());
}

std::uint16_t reserve_port() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    const std::uint16_t port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

void send_wire(int fd, std::uint16_t port, const std::vector<unsigned char>& bytes) {
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::sendto(fd, &bytes[0], bytes.size(), 0,
                    reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
           static_cast<ssize_t>(bytes.size()));
}

void test_market_data_stream_live_replay_parity() {
    char name[] = "/tmp/sse-stream-processor-XXXXXX";
    char* directory = ::mkdtemp(name);
    assert(directory != 0);
    const std::string recording = std::string(directory) + "/capture";
    const std::uint16_t port = reserve_port();
    deepwin_market_data::ChannelSpec channel;
    channel.name = "sse_loopback";
    channel.group = "127.0.0.1";
    channel.interface_ip = "127.0.0.1";
    channel.port = port;
    std::vector<deepwin_market_data::ChannelSpec> channels(1U, channel);
    deepwin_market_data::StreamOptions options;
    options.recording_directory = recording;
    options.recording_required = true;
    options.max_datagram_bytes = 1024U;
    options.segment_bytes = 4096U;
    options.idle_gap_ns = 100000U;
    options.flush_interval_ms = 1;

    std::vector<sse_stream::Output> live_outputs;
    sse_stream::SseStreamProcessor live_processor(
        metadata(), 0, true,
        [&live_outputs](const sse_stream::Output& output) { live_outputs.push_back(output); });
    deepwin_market_data::MarketDataStream live_stream;
    std::string run_error;
    bool run_ok = false;
    std::atomic<bool> ready(false);
    std::thread runner([&]() {
        run_ok = live_stream.run(channels, options,
            [&live_processor, &ready](const deepwin_market_data::StreamEvent& value) {
                ready.store(true);
                live_processor.on_event(value);
            }, 300, &run_error);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(sender >= 0);
    std::vector<unsigned char> bid = tick(1U, 1U, 'A', 0, 9300000U, 1001U, 0U);
    std::vector<unsigned char> ask = tick(2U, 2U, 'A', 1, 9300000U, 0U, 2001U);
    bid.insert(bid.end(), ask.begin(), ask.end());
    // A valid first record doubles as readiness handshake, avoiding a race
    // between the receiver socket setup and the loopback sender.
    for (int attempt = 0; attempt < 100 && !ready.load(); ++attempt) {
        send_wire(sender, port, tick(1U, 1U, 'A', 0, 9300000U, 1001U, 0U));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(ready.load());
    send_wire(sender, port, ask);
    send_wire(sender, port, tick(3U, 3U, 'T', 0, 9300100U, 1001U, 2001U, 100000U));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    send_wire(sender, port, tick(4U, 4U, 'T', 0, 9300200U, 1001U, 2001U, 100000U));
    ::close(sender);
    runner.join();
    assert(run_ok && run_error.empty());
    assert(!live_processor.invalid());

    std::vector<sse_stream::Output> replay_outputs;
    sse_stream::SseStreamProcessor replay_processor(
        metadata(), 0, true,
        [&replay_outputs](const sse_stream::Output& output) { replay_outputs.push_back(output); });
    deepwin_market_data::MarketDataStream replay_stream;
    std::string replay_error;
    assert(replay_stream.replay(recording,
        [&replay_processor](const deepwin_market_data::StreamEvent& value) {
            replay_processor.on_event(value);
        }, &replay_error));
    assert(replay_error.empty());
    assert(live_outputs.size() == replay_outputs.size());
    assert(!live_outputs.empty());
    for (std::size_t i = 0; i < live_outputs.size(); ++i) {
        assert(live_outputs[i].kind == replay_outputs[i].kind);
        if (live_outputs[i].kind == sse_stream::kTickOutput) {
            assert(live_outputs[i].tick.event.tick_index == replay_outputs[i].tick.event.tick_index);
            assert(live_outputs[i].tick.provenance.wire_channel_no == replay_outputs[i].tick.provenance.wire_channel_no);
            assert(live_outputs[i].tick.provenance.wire_sequence == replay_outputs[i].tick.provenance.wire_sequence);
            assert(live_outputs[i].tick.provenance.record_offset == replay_outputs[i].tick.provenance.record_offset);
            assert(live_outputs[i].tick.factors.values == replay_outputs[i].tick.factors.values);
            assert(live_outputs[i].tick.bid_levels.size() == replay_outputs[i].tick.bid_levels.size());
            assert(live_outputs[i].tick.ask_levels.size() == replay_outputs[i].tick.ask_levels.size());
        } else {
            assert(live_outputs[i].snapshot.snapshot.sequence == replay_outputs[i].snapshot.snapshot.sequence);
            assert(live_outputs[i].snapshot.provenance.record_offset == replay_outputs[i].snapshot.provenance.record_offset);
            assert(live_outputs[i].snapshot.snapshot36 == replay_outputs[i].snapshot.snapshot36);
        }
    }
    remove_recording(recording);
    ::rmdir(directory);
}

}  // namespace

void test_unaligned_raw_decode() {
    std::uint64_t seed=0x912345678abcdefULL;
    auto next=[&](){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;};
    for(unsigned alignment=0;alignment<64;++alignment)for(unsigned trial=0;trial<64;++trial) {
        std::vector<unsigned char> storage(alignment+72,0);auto*p=storage.data()+alignment;
        auto put=[&](unsigned offset,std::uint64_t v,unsigned width){for(unsigned j=0;j<width;++j)p[offset+j]=v>>(8*j);};
        const std::uint32_t provider=next(),price=next();
        const auto wire=next(),buy=next(),sell=next(),qty=next(),amount=next();
        put(0,provider,4);p[8]=0x3e;put(9,wire,8);put(17,0x1234,2);
        std::memcpy(p+21,"603686",6);put(30,9300000,4);p[34]="ADTS"[trial%4];
        put(35,buy,8);put(43,sell,8);put(51,price,4);put(55,qty,8);put(63,amount,8);p[71]=trial%2;
        sse_live::RawTickEvent raw;assert(sse_live::decode_primary_raw_tick(p,72,&raw));
        assert(raw.security_number==603686 && raw.channel_no==0x1234 && raw.provider_sequence==provider);
        assert(raw.tick_index==wire && raw.app_seq_num==wire && raw.time_of_day_micros==34200000000ULL);
        assert(raw.buy_order_no==buy && raw.sell_order_no==sell && raw.price_raw==price);
        assert(raw.quantity_raw==qty && raw.amount_raw==amount && raw.event_type==p[34] && raw.side==p[71]);
        sse_live::TickEvent decoded;assert(sse_live::decode_primary_tick(p,72,&decoded));
        assert(decoded.security_id=="603686" && decoded.tick_index==wire && decoded.amount_raw==amount);
        assert(!sse_live::decode_primary_raw_tick(p,trial%72,&raw));
        p[34]='Z';assert(!sse_live::decode_primary_raw_tick(p,72,&raw));
    }
}

int main() {
    test_unaligned_raw_decode();
    test_tick_batch_and_provenance();
    test_hardware_batch_end_marker();
    test_hardware_batches_are_per_channel();
    test_hardware_timestamp_regression_is_per_channel();
    test_duplicate_and_gap_are_distinct();
    test_unconfigured_market_records_count_for_sequence_only();
    test_per_instrument_quiet_stock_closes_while_other_updates();
    test_same_exchange_time_duplicate_bursts_accumulate();
    test_preopen_does_not_seed_tick_window();
    test_snapshot36_and_parity();
    test_snapshot_boundaries_and_regression();
    test_malformed_is_sticky();
    test_primary_heartbeat();
    test_callback_exception_is_sticky();
    test_static_metadata_validation();
    test_synthetic_model_wiring();
    test_market_data_stream_live_replay_parity();
    std::cout << "sse_stream_processor_test: PASS\n";
    return 0;
}
