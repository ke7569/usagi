#include "../strategy/sse_model_consumer.h"
#include "../strategy/sse_tick_prediction_engine.h"
#include "../market_data/sse_primary_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

const std::uint64_t kTickTimeMicros = 34560000000ULL;  // 09:36:00
const std::uint64_t kRealtimeBaseNs = 1700000000000000000ULL;
const std::uint64_t kMonotonicBaseNs = 9000000000000000000ULL;
const std::uint64_t kAddQuantityRaw = 200000ULL;
const std::uint64_t kTradeQuantityRaw = 100000ULL;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void put_u16(std::vector<unsigned char>* bytes, std::size_t offset,
             std::uint32_t value) {
    (*bytes)[offset] = static_cast<unsigned char>(value & 0xffU);
    (*bytes)[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void put_u32(std::vector<unsigned char>* bytes, std::size_t offset,
             std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        (*bytes)[offset + index] = static_cast<unsigned char>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void put_u64(std::vector<unsigned char>* bytes, std::size_t offset,
             std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        (*bytes)[offset + index] = static_cast<unsigned char>(
            (value >> (index * 8U)) & 0xffU);
    }
}

std::vector<unsigned char> raw_tick(const std::string& security,
                                    std::uint16_t channel,
                                    std::uint64_t tick_index,
                                    char event_type,
                                    std::uint64_t buy_order,
                                    std::uint64_t sell_order,
                                    std::uint32_t price_raw,
                                    std::uint64_t quantity_raw,
                                    unsigned char side) {
    check(security.size() == 6U, "test security must be six digits");
    std::vector<unsigned char> bytes(72U, 0U);
    bytes[8U] = 0x3eU;
    put_u32(&bytes, 0U, static_cast<std::uint32_t>(tick_index + 100U));
    put_u64(&bytes, 9U, tick_index);
    put_u16(&bytes, 17U, channel);
    std::memcpy(&bytes[21U], security.data(), security.size());
    // 09:36:00.00 in HHMMSScc format.
    put_u32(&bytes, 30U, 9360000U);
    bytes[34U] = static_cast<unsigned char>(event_type);
    put_u64(&bytes, 35U, buy_order);
    put_u64(&bytes, 43U, sell_order);
    put_u32(&bytes, 51U, price_raw);
    put_u64(&bytes, 55U, quantity_raw);
    put_u64(&bytes, 63U, static_cast<std::uint64_t>(price_raw) * quantity_raw);
    bytes[71U] = side;
    return bytes;
}

std::vector<unsigned char> raw_snapshot(const std::string& security) {
    check(security.size() == 6U, "test snapshot security must be six digits");
    std::vector<unsigned char> bytes(440U, 0U);
    bytes[8U] = 0x27U;
    put_u32(&bytes, 0U, 700001U);
    put_u32(&bytes, 21U, 800001U);
    put_u32(&bytes, 26U, 93600U);
    std::memcpy(&bytes[30U], security.data(), security.size());
    put_u32(&bytes, 42U, 10000U);
    put_u32(&bytes, 46U, 10000U);
    put_u32(&bytes, 50U, 10200U);
    put_u32(&bytes, 54U, 9800U);
    put_u32(&bytes, 58U, 10100U);
    put_u64(&bytes, 74U, 200000000ULL);
    put_u64(&bytes, 82U, 20000000000ULL);
    for (std::size_t level = 0U; level < 5U; ++level) {
        const std::size_t bid = 124U + level * 16U;
        const std::size_t ask = 124U + (10U + level) * 16U;
        put_u32(&bytes, bid, 10000U - static_cast<std::uint32_t>(level * 10U));
        put_u64(&bytes, bid + 4U, 100000ULL);
        put_u32(&bytes, ask, 10100U + static_cast<std::uint32_t>(level * 10U));
        put_u64(&bytes, ask + 4U, 100000ULL);
    }
    return bytes;
}

sse_pipeline::Event make_event(const std::string& security,
                               std::uint16_t channel,
                               std::uint32_t shard,
                               std::uint64_t event_id,
                               std::uint64_t tick_index,
                               char event_type,
                               std::uint64_t buy_order,
                               std::uint64_t sell_order,
                               std::uint32_t price_raw,
                               std::uint64_t quantity_raw,
                               unsigned char side) {
    sse_pipeline::Event event;
    std::memset(&event, 0, sizeof(event));
    event.event_id = event_id;
    event.receive_realtime_ns = kRealtimeBaseNs + tick_index * 1000000ULL;
    event.receive_mono_ns = kMonotonicBaseNs + tick_index * 1000000ULL;
    event.exchange_time_us = kTickTimeMicros;
    event.sequence = tick_index;
    event.trading_day = 0U;
    event.shard_id = shard;
    event.channel_no = channel;
    event.payload_size = 72U;
    event.kind = static_cast<std::uint8_t>(sse_pipeline::kTick);
    event.flags = 0U;
    event.version = static_cast<std::uint16_t>(sse_pipeline::kEventVersion);
    std::memcpy(event.security_id, security.data(), security.size());
    const std::vector<unsigned char> payload = raw_tick(
        security, channel, tick_index, event_type, buy_order, sell_order,
        price_raw, quantity_raw, side);
    std::memcpy(event.payload, payload.data(), payload.size());
    return event;
}

sse_pipeline::Event make_marker(const sse_pipeline::Event& previous,
                                std::uint64_t event_id,
                                std::uint64_t exchange_time_us = kTickTimeMicros) {
    sse_pipeline::Event marker = previous;
    marker.event_id = event_id;
    marker.receive_realtime_ns = previous.receive_realtime_ns;
    marker.receive_mono_ns = previous.receive_mono_ns + 101000ULL;
    marker.exchange_time_us = exchange_time_us;
    marker.sequence = previous.sequence;
    marker.kind = static_cast<std::uint8_t>(sse_pipeline::kTickSample);
    return marker;
}

sse_pipeline::Event make_snapshot_event(const std::string& security,
                                        std::uint64_t event_id) {
    sse_pipeline::Event event;
    std::memset(&event, 0, sizeof(event));
    event.event_id = event_id;
    event.receive_realtime_ns = kRealtimeBaseNs + 9000000ULL;
    event.receive_mono_ns = kMonotonicBaseNs + 9000000ULL;
    event.exchange_time_us = kTickTimeMicros;
    event.sequence = 800001U;
    event.trading_day = 0U;
    event.shard_id = sse_pipeline::kUnassignedShard;
    event.channel_no = 0U;
    event.payload_size = 440U;
    event.kind = static_cast<std::uint8_t>(sse_pipeline::kSnapshot);
    event.flags = 0U;
    event.version = static_cast<std::uint16_t>(sse_pipeline::kEventVersion);
    std::memcpy(event.security_id, security.data(), security.size());
    const std::vector<unsigned char> payload = raw_snapshot(security);
    std::memcpy(event.payload, payload.data(), payload.size());
    return event;
}

std::vector<sse_pipeline::Event> make_stream(const std::string& security,
                                             std::uint16_t channel,
                                             std::uint32_t shard) {
    std::vector<sse_pipeline::Event> stream;
    stream.push_back(make_event(security, channel, shard, 1U, 1U, 'A',
                                1001U, 0U, 10000U, kAddQuantityRaw, 0U));
    stream.push_back(make_event(security, channel, shard, 2U, 2U, 'A',
                                0U, 2001U, 10100U, kAddQuantityRaw, 1U));
    stream.push_back(make_event(security, channel, shard, 3U, 3U, 'T',
                                1001U, 2001U, 10000U, kTradeQuantityRaw, 0U));
    stream.push_back(make_marker(stream.back(), 4U));
    stream.push_back(make_event(security, channel, shard, 5U, 4U, 'A',
                                1002U, 0U, 10100U, kAddQuantityRaw, 0U));
    stream.push_back(make_event(security, channel, shard, 6U, 5U, 'A',
                                0U, 2002U, 10200U, kAddQuantityRaw, 1U));
    stream.push_back(make_event(security, channel, shard, 7U, 6U, 'T',
                                1002U, 2002U, 10100U, kTradeQuantityRaw, 0U));
    stream.push_back(make_marker(stream.back(), 8U));
    return stream;
}

void set_trading_day(std::vector<sse_pipeline::Event>* stream,
                     std::uint32_t trading_day) {
    for (std::size_t index = 0U; index < stream->size(); ++index)
        (*stream)[index].trading_day = trading_day;
}

std::uint32_t parse_day(const char* text) {
    if (text == 0 || *text == '\0') throw std::runtime_error("missing trading day");
    char* end = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value < 20000101UL || value > 99991231UL)
        throw std::runtime_error("invalid trading day");
    return static_cast<std::uint32_t>(value);
}

void decode_tick(const sse_pipeline::Event& event, sse_live::TickEvent* tick) {
    std::string error;
    check(sse_live::decode_primary_tick(event.payload, event.payload_size, tick, &error),
          std::string("fixture tick decode failed: ") + error);
}

sse_pipeline::ModelOptions model_options(const std::string& model_path,
                                         const std::string& static_path,
                                         std::uint32_t trading_day) {
    sse_pipeline::ModelOptions options;
    options.tick_model = model_path;
    options.static_json = static_path;
    options.trading_day = trading_day;
    options.snapshot_enabled = false;
    return options;
}

std::pair<std::string, std::string> choose_stocks(
    const sse_tick::DailyStaticMetadataMap& metadata) {
    std::string first;
    std::string second;
    for (sse_tick::DailyStaticMetadataMap::const_iterator it = metadata.begin();
         it != metadata.end(); ++it) {
        if (!sse_live::is_sse_stock(it->first) || !it->second.complete()) continue;
        if (first.empty()) first = it->first;
        else if (it->first != first) {
            second = it->first;
            break;
        }
    }
    check(!first.empty() && !second.empty(),
          "static metadata must contain two complete SSE stocks");
    return std::make_pair(first, second);
}

double reference_prediction_sum(const std::string& model_path,
                                const std::string& symbol,
                                const sse_tick::DailyStaticMetadata& metadata,
                                const std::vector<sse_pipeline::Event>& stream,
                                bool with_snapshot_aux) {
    sse_tick_strategy::PredictionEngine engine;
    std::string error;
    check(engine.load(model_path, &error),
          std::string("reference tick model load failed: ") + error);
    check(engine.set_static_metadata(symbol, metadata),
          "reference static metadata setup failed");
    double sum = 0.0;
    for (std::size_t index = 0U; index < stream.size(); ++index) {
        const sse_pipeline::Event& event = stream[index];
        if (event.kind == static_cast<std::uint8_t>(sse_pipeline::kTick)) {
            sse_live::TickEvent tick;
            decode_tick(event, &tick);
            sse_tick_strategy::PredictionRow row;
            check(engine.observe(tick, event.receive_mono_ns / 1000ULL, 0U,
                                  &row, &error),
                  std::string("reference tick observe failed: ") + error);
        } else if (event.kind == static_cast<std::uint8_t>(sse_pipeline::kTickSample)) {
            if (with_snapshot_aux) {
                const sse_pipeline::Event snapshot_event = make_snapshot_event(symbol, 100U + event.event_id);
                sse_live::Snapshot snapshot;
                check(sse_live::decode_primary_snapshot(
                          snapshot_event.payload, snapshot_event.payload_size,
                          &snapshot, &error),
                      std::string("fixture snapshot decode failed: ") + error);
                check(engine.observe_snapshot(snapshot, &error),
                      std::string("reference snapshot auxiliary update failed: ") + error);
            }
            sse_live::TickEvent tick;
            decode_tick(event, &tick);
            const std::uint64_t boundary_us = event.receive_mono_ns / 1000ULL;
            sse_tick_strategy::PredictionRow row;
            check(engine.sample_tick(tick, boundary_us - 101ULL, boundary_us,
                                     &row, &error),
                  std::string("reference sample failed: ") + error);
            check(!row.security_id.empty() && row.model_valid,
                  "reference sample did not produce a model prediction");
            sum += static_cast<double>(row.prediction);
        }
    }
    return sum;
}

void consume_stream(sse_pipeline::ModelConsumer* consumer,
                    const std::vector<sse_pipeline::Event>& stream,
                    const std::string& context) {
    for (std::size_t index = 0U; index < stream.size(); ++index) {
        check(consumer->consume(stream[index]), context + " event rejected: " + consumer->error());
    }
}

void assert_close(double actual, double expected, const std::string& message) {
    const double tolerance = 1.0e-5 * std::max(1.0, std::fabs(expected));
    check(std::fabs(actual - expected) <= tolerance,
          message + " actual=" + std::to_string(actual) +
              " expected=" + std::to_string(expected));
}

void test_consumer_and_state_isolation(
    const std::string& model_path, const std::string& static_path,
    std::uint32_t trading_day,
    const std::pair<std::string, std::string>& stocks,
    const sse_tick::DailyStaticMetadataMap& metadata) {
    std::vector<sse_pipeline::Event> first = make_stream(stocks.first, 1U, 0U);
    std::vector<sse_pipeline::Event> second = make_stream(stocks.second, 2U, 1U);
    set_trading_day(&first, trading_day);
    set_trading_day(&second, trading_day);
    const double expected_first = reference_prediction_sum(
        model_path, stocks.first, metadata.find(stocks.first)->second, first, false);
    const double expected_second = reference_prediction_sum(
        model_path, stocks.second, metadata.find(stocks.second)->second, second, false);

    sse_pipeline::ModelConsumer first_consumer(model_options(
        model_path, static_path, trading_day));
    std::string error;
    check(first_consumer.load(&error), "single-stock consumer load failed: " + error);
    consume_stream(&first_consumer, first, "single-stock");
    const sse_pipeline::ModelStats& first_stats = first_consumer.stats();
    check(first_stats.predictions == 2U && first_stats.model_errors == 0U,
          "single-stock consumer prediction statistics are invalid");
    check(first_stats.selected == 1U,
          "09:36 second tick sample was not selected");
    check(first_consumer.stock_predictions().find(stocks.first) !=
              first_consumer.stock_predictions().end() &&
              first_consumer.stock_predictions().find(stocks.first)->second == 2U,
          "single-stock prediction count is invalid");
    assert_close(first_stats.prediction_sum, expected_first,
                 "single-stock prediction differs from direct engine");

    sse_pipeline::ModelConsumer interleaved(model_options(
        model_path, static_path, trading_day));
    check(interleaved.load(&error), "interleaved consumer load failed: " + error);
    for (std::size_t index = 0U; index < first.size(); ++index) {
        check(interleaved.consume(first[index]), "interleaved first-stock event rejected: " + interleaved.error());
        check(interleaved.consume(second[index]), "interleaved second-stock event rejected: " + interleaved.error());
    }
    const sse_pipeline::ModelStats& interleaved_stats = interleaved.stats();
    check(interleaved_stats.predictions == 4U && interleaved_stats.model_errors == 0U,
          "interleaved consumer prediction statistics are invalid");
    check(interleaved_stats.selected == 2U,
          "interleaved 09:36 predictions were not selected");
    check(interleaved.stock_predictions().find(stocks.first) !=
              interleaved.stock_predictions().end() &&
              interleaved.stock_predictions().find(stocks.first)->second == 2U &&
              interleaved.stock_predictions().find(stocks.second) !=
              interleaved.stock_predictions().end() &&
              interleaved.stock_predictions().find(stocks.second)->second == 2U,
          "interleaved per-stock prediction counts are invalid");
    assert_close(interleaved_stats.prediction_sum,
                 expected_first + expected_second,
                 "interleaved prediction sum differs from independent engines");
}

void test_warmup_and_malformed_marker(
    const std::string& model_path, const std::string& static_path,
    std::uint32_t trading_day, const std::string& symbol) {
    std::vector<sse_pipeline::Event> stream = make_stream(symbol, 1U, 0U);
    set_trading_day(&stream, trading_day);
    std::string error;
    const sse_pipeline::ModelOptions options = model_options(
        model_path, static_path, trading_day);
    sse_pipeline::ModelConsumer warm(options);
    check(warm.load(&error), "warmup consumer load failed: " + error);
    sse_pipeline::Event early_marker = stream[3U];
    early_marker.exchange_time_us = options.tick_start_us - 1U;
    check(warm.consume(early_marker), "pre-09:25 sample marker was rejected");
    check(warm.stats().samples == 0U && warm.stats().predictions == 0U,
          "pre-09:25 sample marker advanced prediction state");
    consume_stream(&warm, stream, "warmup follow-up");
    check(warm.stats().predictions == 2U && warm.stats().model_errors == 0U,
          "post-warmup samples did not produce predictions");

    sse_pipeline::ModelConsumer malformed(model_options(
        model_path, static_path, trading_day));
    check(malformed.load(&error), "malformed-marker consumer load failed: " + error);
    for (std::size_t index = 0U; index < 3U; ++index)
        check(malformed.consume(stream[index]), "malformed-marker setup tick rejected");
    sse_pipeline::Event bad_marker = stream[3U];
    bad_marker.payload[8U] = 0U;
    check(!malformed.consume(bad_marker), "malformed sample marker was accepted");
    check(malformed.stats().predictions == 0U && !malformed.error().empty(),
          "malformed sample marker changed prediction state without an error");
}

void test_snapshot_auxiliary_does_not_advance_tick_model(
    const std::string& model_path, const std::string& symbol,
    const sse_tick::DailyStaticMetadata& metadata,
    std::uint32_t trading_day) {
    std::vector<sse_pipeline::Event> stream = make_stream(symbol, 1U, 0U);
    set_trading_day(&stream, trading_day);
    const double without_snapshot = reference_prediction_sum(
        model_path, symbol, metadata, stream, false);
    const double with_snapshot = reference_prediction_sum(
        model_path, symbol, metadata, stream, true);
    assert_close(with_snapshot, without_snapshot,
                 "snapshot auxiliary update advanced tick model state");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: sse_model_consumer_test TICK_MODEL STATIC_JSON YYYYMMDD\n";
        return 2;
    }
    try {
        const std::string model_path(argv[1]);
        const std::string static_path(argv[2]);
        const std::uint32_t trading_day = parse_day(argv[3]);
        sse_tick::DailyStaticMetadataMap metadata;
        std::string error;
        check(sse_tick::load_daily_static_metadata(
                  static_path, trading_day, &metadata, &error),
              "static metadata load failed: " + error);
        const std::pair<std::string, std::string> stocks = choose_stocks(metadata);
        test_consumer_and_state_isolation(
            model_path, static_path, trading_day, stocks, metadata);
        test_warmup_and_malformed_marker(
            model_path, static_path, trading_day, stocks.first);
        test_snapshot_auxiliary_does_not_advance_tick_model(
            model_path, stocks.first, metadata.find(stocks.first)->second,
            trading_day);
        std::cout << "sse_model_consumer_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
