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

std::vector<unsigned char> snapshot(std::uint32_t sequence,
                                    std::uint32_t time_raw,
                                    std::uint64_t volume,
                                    std::uint64_t turnover) {
    std::vector<unsigned char> bytes(440U, 0U);
    bytes[8] = 0x27U;
    put_u32(&bytes, 0U, sequence);
    put_u32(&bytes, 21U, sequence);
    put_ascii(&bytes, 30U, "600000", 8U);
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
    std::vector<unsigned char> opening = tick(1U, 1U, 'A', 0, 9350000U, 1001U, 0U);
    const std::vector<unsigned char> ask = tick(2U, 2U, 'A', 1, 9350000U, 0U, 2001U);
    opening.insert(opening.end(), ask.begin(), ask.end());
    const std::vector<unsigned char> trade = tick(
        3U, 3U, 'T', 0, 9350000U, 1001U, 2001U, 100000U);
    opening.insert(opening.end(), trade.begin(), trade.end());
    processor.on_event(event(opening, 1U, 1000U));
    processor.on_event(idle(2U, 101001U));
    processor.on_event(event(tick(4U, 4U, 'T', 0, 9350100U,
                              1001U, 2001U, 100000U), 3U, 200000U));
    processor.on_event(idle(4U, 301001U));
    const std::vector<unsigned char> later = tick(
        5U, 5U, 'T', 0, 9350200U, 1001U, 2001U, 100000U);
    processor.on_event(event(later, 5U, 400000U));
    processor.on_event(idle(6U, 501001U));
    assert(outputs.size() >= 2U);
    assert(outputs[0].kind == sse_stream::kTickOutput);
    assert(outputs[0].tick.prediction_valid);
    assert(!outputs[0].tick.prediction.selected);  // incomplete seeded row
    assert(outputs[1].tick.prediction_valid);
    assert(outputs[1].tick.prediction.selected);
    assert(outputs[1].tick.prediction.selected_source == sse_hybrid_model::kTickSource);

    const std::vector<unsigned char> snap1 = snapshot(10U, 93000U, 1000U, 1000U);
    const std::vector<unsigned char> snap2 = snapshot(11U, 93100U, 1100U, 1100U);
    processor.on_event(event(snap1, 7U, 600000U));
    processor.on_event(event(snap2, 8U, 700000U));
    assert(outputs.size() >= 3U);
    assert(outputs.back().kind == sse_stream::kSnapshotOutput);
    assert(outputs.back().snapshot.prediction_valid);
    assert(outputs.back().snapshot.prediction.selected);
    assert(outputs.back().snapshot.auction59.size() == 59U);

    bool threw = false;
    try {
        sse_stream::SseStreamProcessor missing_provider(
            metadata(), &model, false, [](const sse_stream::Output&) {});
    } catch (const std::runtime_error&) { threw = true; }
    assert(threw);
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

int main() {
    test_tick_batch_and_provenance();
    test_duplicate_and_gap_are_distinct();
    test_unconfigured_market_records_count_for_sequence_only();
    test_per_instrument_quiet_stock_closes_while_other_updates();
    test_same_exchange_time_duplicate_bursts_accumulate();
    test_preopen_does_not_seed_tick_window();
    test_snapshot36_and_parity();
    test_snapshot_boundaries_and_regression();
    test_malformed_is_sticky();
    test_callback_exception_is_sticky();
    test_static_metadata_validation();
    test_synthetic_model_wiring();
    test_market_data_stream_live_replay_parity();
    std::cout << "sse_stream_processor_test: PASS\n";
    return 0;
}
