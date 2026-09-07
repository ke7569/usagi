#include "sze/runtime/sze_recovery_driver.h"

#include "sze/market_data/SZEProtocol.h"
#include "sze/market_data/SZERecoverable.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        char name[] = "/tmp/sze-recovery-processor-XXXXXX";
        char* result = ::mkdtemp(name);
        assert(result != 0);
        path = result;
    }
    ~TemporaryDirectory() {
        DIR* directory = ::opendir(path.c_str());
        if (directory != 0) {
            for (;;) {
                dirent* entry = ::readdir(directory);
                if (entry == 0) break;
                if (!std::strcmp(entry->d_name, ".") ||
                    !std::strcmp(entry->d_name, "..")) continue;
                ::unlink((path + "/" + entry->d_name).c_str());
            }
            ::closedir(directory);
        }
        ::rmdir(path.c_str());
    }
    std::string path;
};

void set_symbol(std::uint8_t* destination, const char* value) {
    std::memset(destination, 0, 9U);
    std::memcpy(destination, value, std::strlen(value));
}

sze_md::SzeHpfHead make_head(std::uint8_t message_type,
                             std::uint32_t feed_sequence,
                             std::uint64_t application_sequence,
                             std::uint64_t exchange_time) {
    sze_md::SzeHpfHead head = {};
    head.sequence = feed_sequence;
    head.message_type = message_type;
    head.security_type = 1U;
    set_symbol(head.symbol, "000001");
    head.exchange_id = 101U;
    head.quote_update_time = exchange_time;
    head.channel_num = 7U;
    head.sequence_num = application_sequence;
    head.md_stream_id = 1U;
    return head;
}

std::uint64_t exchange_time(std::uint64_t seconds_after_open) {
    const std::uint64_t total = 9U * 3600U + 30U * 60U + seconds_after_open;
    return 20260720ULL * 1000000000ULL +
        (total / 3600U) * 10000000ULL +
        ((total / 60U) % 60U) * 100000ULL +
        (total % 60U) * 1000ULL;
}

std::vector<unsigned char> make_order(std::uint32_t feed,
                                      std::uint64_t application,
                                      bool buy,
                                      std::uint64_t time) {
    sze_md::SzeHpfOrder order = {};
    order.head = make_head(sze_md::kOrderMessage, feed, application, time);
    order.order_price = 100000U;
    order.order_quantity = 10000U;
    order.side_flag = buy ? '1' : '2';
    order.order_type = '2';
    return std::vector<unsigned char>(
        reinterpret_cast<unsigned char*>(&order),
        reinterpret_cast<unsigned char*>(&order) + sizeof(order));
}

std::vector<unsigned char> make_execution(std::uint32_t feed,
                                          std::uint64_t application,
                                          std::uint64_t buy_id,
                                          std::uint64_t sell_id,
                                          std::uint64_t time) {
    sze_md::SzeHpfExecution execution = {};
    execution.head = make_head(sze_md::kExecutionMessage, feed, application, time);
    execution.trade_buy_num = static_cast<std::int64_t>(buy_id);
    execution.trade_sell_num = static_cast<std::int64_t>(sell_id);
    execution.trade_price = 100000U;
    execution.trade_quantity = 10000;
    execution.trade_type = 'F';
    return std::vector<unsigned char>(
        reinterpret_cast<unsigned char*>(&execution),
        reinterpret_cast<unsigned char*>(&execution) + sizeof(execution));
}

mix153060::StaticInputs inputs() {
    mix153060::StaticInputs result;
    result.instrument = "000001.SZ";
    result.trading_date = 20260720;
    result.average_amount = 1000000.0;
    result.turnover_threshold = 1000000000.0;
    result.free_share = 1000000.0;
    result.pre_close = 10.0;
    result.upper_limit = 11.0;
    result.lower_limit = 9.0;
    result.history_volatility_20d = 0.2;
    return result;
}

struct RecoveryRecord {
    sze_recovery::CanonicalEvent event;
    std::vector<unsigned char> payload;
};

RecoveryRecord make_recovery_record(std::uint64_t event_id,
                                    std::uint32_t feed,
                                    std::uint64_t application,
                                    const std::vector<unsigned char>& payload,
                                    std::uint64_t time) {
    RecoveryRecord result;
    std::memset(&result.event, 0, sizeof(result.event));
    result.event.event_id = event_id;
    result.event.feed_sequence = feed;
    result.event.channel_sequence = application;
    result.event.receive_mono_ns = event_id * 1000U;
    result.event.exchange_time = time;
    result.event.trading_day = 20260720U;
    result.event.source_id = 88U;
    result.event.channel_number = 7U;
    result.event.payload_size = static_cast<std::uint16_t>(payload.size());
    result.event.message_type = payload[8];
    result.event.record_kind = sze_recovery::kRecordMarketData;
    result.event.payload_crc32 = sze_recovery::crc32(payload.data(), payload.size());
    result.payload = payload;
    return result;
}

std::int64_t exchange_us(const char* text) {
    std::int64_t result = 0;
    assert(mix153060::parse_exchange_time_us(text, 20260720, &result));
    return result;
}

deepwin_market_data::StreamEvent raw_event(const std::vector<unsigned char>& payload,
                                           std::uint64_t sequence,
                                           std::uint64_t time) {
    deepwin_market_data::StreamEvent event = {};
    event.kind = deepwin_market_data::kDatagramEvent;
    event.sequence = sequence;
    event.realtime_ns = time;
    event.monotonic_ns = time;
    event.channel_id = 0U;
    event.data = payload.data();
    event.size = payload.size();
    return event;
}

void compare_samples(const std::vector<sze_stream::ProcessedSample>& left,
                     const std::vector<sze_stream::ProcessedSample>& right) {
    assert(left.size() == right.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        assert(left[i].sample.instrument == right[i].sample.instrument);
        assert(left[i].sample.exchange_time_us == right[i].sample.exchange_time_us);
        assert(left[i].sample.local_time_us == right[i].sample.local_time_us);
        assert(left[i].sample.app_sequence == right[i].sample.app_sequence);
        assert(left[i].sample.cut_index == right[i].sample.cut_index);
        for (std::size_t j = 0; j < left[i].sample.factors.size(); ++j) {
            assert(left[i].sample.factors[j] == right[i].sample.factors[j]);
        }
        for (std::size_t j = 0; j < left[i].sample.bid_price.size(); ++j) {
            assert(left[i].sample.bid_price[j] == right[i].sample.bid_price[j]);
            assert(left[i].sample.ask_price[j] == right[i].sample.ask_price[j]);
            assert(left[i].sample.bid_volume[j] == right[i].sample.bid_volume[j]);
            assert(left[i].sample.ask_volume[j] == right[i].sample.ask_volume[j]);
        }
    }
}

void write_fixture(const std::string& directory,
                   const std::vector<RecoveryRecord>& records) {
    sze_recovery::JournalConfig config;
    config.directory = directory;
    config.prefix = "fixture";
    config.trading_day = 20260720U;
    config.source_id = 88U;
    config.segment_bytes = 1U << 20U;
    config.max_payload_bytes = 256U;
    sze_recovery::JournalWriter writer;
    assert(writer.open(config).status == sze_recovery::kJournalOk);
    for (std::size_t i = 0; i < records.size(); ++i) {
        sze_recovery::CanonicalEvent event = records[i].event;
        assert(writer.append(&event, records[i].payload.data()) ==
               sze_recovery::kJournalOk);
    }
    assert(writer.publish_continuity(sze_recovery::kContinuityValid,
                                     sze_recovery::kInvalidNone,
                                     records.empty() ? 0U : records.back().event.feed_sequence) ==
           sze_recovery::kJournalOk);
    assert(writer.close(true) == sze_recovery::kJournalOk);
}

std::vector<RecoveryRecord> make_records() {
    std::vector<RecoveryRecord> records;
    records.push_back(make_recovery_record(
        1U, 100U, 1U, make_order(100U, 1U, true, exchange_time(0)),
        exchange_time(0)));
    records.push_back(make_recovery_record(
        2U, 102U, 2U, make_order(102U, 2U, false, exchange_time(1)),
        exchange_time(1)));
    records.push_back(make_recovery_record(
        3U, 105U, 3U, make_execution(105U, 3U, 1U, 2U, exchange_time(30)),
        exchange_time(30)));
    records.push_back(make_recovery_record(
        4U, 106U, 4U, make_order(106U, 4U, true, exchange_time(32)),
        exchange_time(32)));
    records.push_back(make_recovery_record(
        5U, 108U, 5U, make_order(108U, 5U, false, exchange_time(33)),
        exchange_time(33)));
    records.push_back(make_recovery_record(
        6U, 111U, 6U, make_execution(111U, 6U, 4U, 5U, exchange_time(131)),
        exchange_time(131)));
    return records;
}

void test_journal_parity_and_filtered_gaps() {
    const std::vector<RecoveryRecord> records = make_records();
    std::vector<sze_stream::ProcessedSample> raw_samples;
    std::vector<sze_stream::ProcessedSample> recovery_samples;
    const std::vector<mix153060::StaticInputs> configured(1, inputs());
    sze_stream::SzeStreamProcessor raw(
        configured, 0, 1,
        [&](const sze_stream::ProcessedSample& sample) {
            raw_samples.push_back(sample);
        });
    std::uint32_t raw_feed_sequence = 99U;
    std::uint64_t ingress_sequence = 0U;
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i != 0U) {
            while (raw_feed_sequence + 1U < records[i].event.feed_sequence) {
                sze_md::SzeHpfHeartbeat heartbeat = {};
                heartbeat.sequence = ++raw_feed_sequence;
                heartbeat.message_type = sze_md::kTickHeartbeatMessage;
                const std::vector<unsigned char> control(
                    reinterpret_cast<unsigned char*>(&heartbeat),
                    reinterpret_cast<unsigned char*>(&heartbeat) + sizeof(heartbeat));
                raw.on_event(raw_event(control, ++ingress_sequence,
                                        static_cast<std::uint64_t>(
                                            exchange_us("09:30:00.000")) * 1000U));
            }
        }
        const std::uint64_t local_ns = static_cast<std::uint64_t>(
            exchange_us(i == 0U ? "09:30:00.000" :
                        i == 1U ? "09:30:00.001" :
                        i == 2U ? "09:30:00.030" :
                        i == 3U ? "09:30:00.032" :
                        i == 4U ? "09:30:00.033" : "09:32:11.000")) * 1000U;
        raw_feed_sequence = records[i].event.feed_sequence;
        raw.on_event(raw_event(records[i].payload, ++ingress_sequence, local_ns));
    }
    assert(raw.available());

    TemporaryDirectory temporary;
    write_fixture(temporary.path, records);
    sze_stream::SzeStreamProcessor recovery(
        configured, 0, 1,
        [&](const sze_stream::ProcessedSample& sample) {
            recovery_samples.push_back(sample);
        });
    sze_stream::SzeRecoveryDriver driver(&recovery);
    sze_stream::SzeRecoveryJournalConfig config;
    config.directory = temporary.path;
    config.prefix = "fixture";
    config.trading_day = 20260720U;
    config.source_id = 88U;
    config.segment_bytes = 1U << 20U;
    config.max_payload_bytes = 256U;
    assert(driver.replay(config, sze_stream::RecoveryTimeContext::analysis_exchange_time()));
    assert(driver.available() && driver.stats().records == records.size());
    assert(recovery_samples.size() == raw_samples.size());
    for (std::size_t i = 0; i < recovery_samples.size(); ++i) {
        assert(recovery_samples[i].recovery_event_id != 0U);
        assert(recovery_samples[i].recovery_feed_sequence != 0U);
        compare_samples(std::vector<sze_stream::ProcessedSample>(1, raw_samples[i]),
                        std::vector<sze_stream::ProcessedSample>(1, recovery_samples[i]));
    }
}

void test_invalid_recovery_is_sticky() {
    const RecoveryRecord good = make_records()[0];
    const std::vector<mix153060::StaticInputs> configured(1, inputs());
    {
        sze_stream::SzeStreamProcessor processor(configured, 0, 1);
        sze_recovery::CanonicalEvent bad = good.event;
        bad.source_id = 89U;
        bool threw = false;
        try {
            processor.on_recovery(bad, good.payload.data(), good.payload.size(),
                                  sze_stream::RecoveryTimeContext::analysis_exchange_time());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && !processor.available());
        assert(!processor.error().empty());
    }
    {
        sze_stream::SzeStreamProcessor processor(configured, 0, 1);
        sze_recovery::CanonicalEvent bad = good.event;
        bad.trading_day = 20260721U;
        bool threw = false;
        try {
            processor.on_recovery(bad, good.payload.data(), good.payload.size(),
                                  sze_stream::RecoveryTimeContext::analysis_exchange_time());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && !processor.available());
    }
    {
        sze_stream::SzeStreamProcessor processor(configured, 0, 1);
        std::vector<unsigned char> bad_payload = good.payload;
        bad_payload[20] ^= 0x01U;
        bool threw = false;
        try {
            processor.on_recovery(good.event, bad_payload.data(), bad_payload.size(),
                                  sze_stream::RecoveryTimeContext::analysis_exchange_time());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && !processor.available());
    }
    {
        sze_stream::SzeStreamProcessor processor(configured, 0, 1);
        processor.on_recovery(good.event, good.payload.data(), good.payload.size(),
                              sze_stream::RecoveryTimeContext::analysis_exchange_time());
        sze_recovery::CanonicalEvent gap = good.event;
        gap.event_id = 3U;
        bool threw = false;
        try {
            processor.on_recovery(gap, good.payload.data(), good.payload.size(),
                                  sze_stream::RecoveryTimeContext::analysis_exchange_time());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && !processor.available());
    }
}

void test_no_implicit_eof_flush_and_clock_guard() {
    const RecoveryRecord good = make_records()[0];
    const std::vector<mix153060::StaticInputs> configured(1, inputs());
    TemporaryDirectory temporary;
    write_fixture(temporary.path, std::vector<RecoveryRecord>(1, good));
    sze_stream::SzeStreamProcessor processor(configured, 0, 1);
    sze_stream::SzeRecoveryDriver driver(&processor);
    sze_stream::SzeRecoveryJournalConfig config;
    config.directory = temporary.path;
    config.prefix = "fixture";
    config.trading_day = 20260720U;
    config.source_id = 88U;
    config.segment_bytes = 1U << 20U;
    assert(driver.replay(config, sze_stream::RecoveryTimeContext::analysis_exchange_time()));
    assert(processor.stats().samples == 0U);

    sze_stream::SzeStreamProcessor clock_processor(configured, 0, 1);
    bool threw = false;
    try {
        clock_processor.on_recovery(
            good.event, good.payload.data(), good.payload.size(),
            sze_stream::RecoveryTimeContext::same_boot(0U, 1U));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && !clock_processor.available());
}

void test_generation_rejection() {
    TemporaryDirectory temporary;
    write_fixture(temporary.path, make_records());
    sze_stream::SzeStreamProcessor processor(
        std::vector<mix153060::StaticInputs>(1, inputs()), 0, 1);
    sze_stream::SzeRecoveryDriver driver(&processor);
    sze_stream::SzeRecoveryJournalConfig config;
    config.directory = temporary.path;
    config.prefix = "fixture";
    config.trading_day = 20260720U;
    config.source_id = 88U;
    config.segment_bytes = 1U << 20U;
    config.expected_generation = 1U;
    assert(!driver.replay(config, sze_stream::RecoveryTimeContext::analysis_exchange_time()));
    assert(!driver.available());
}

void test_handoff_cutover_and_empty_timeout() {
    const std::vector<RecoveryRecord> records = make_records();
    TemporaryDirectory temporary;
    const std::string ring_path = temporary.path + "/handoff.shm";
    sze_recovery::JournalConfig journal;
    journal.directory = temporary.path;
    journal.prefix = "handoff";
    journal.trading_day = 20260720U;
    journal.source_id = 88U;
    journal.segment_bytes = 1U << 20U;
    journal.max_payload_bytes = 256U;
    sze_recovery::JournalWriter writer;
    assert(writer.open(journal).status == sze_recovery::kJournalOk);
    assert(writer.publish_continuity(sze_recovery::kContinuityValid,
                                     sze_recovery::kInvalidNone, 0U) ==
           sze_recovery::kJournalOk);
    sze_recovery::RingConfig ring_config;
    ring_config.path = ring_path;
    ring_config.trading_day = journal.trading_day;
    ring_config.source_id = journal.source_id;
    ring_config.capacity = 32U;
    ring_config.max_payload_bytes = 256U;
    ring_config.generation = writer.generation();
    sze_recovery::ShmEventRing producer;
    assert(producer.create(ring_config));
    producer.publish_state(sze_recovery::kContinuityValid,
                           sze_recovery::kReadinessNotReady,
                           sze_recovery::kInvalidNone, 0U, 0U);
    for (std::size_t i = 0; i < records.size(); ++i) {
        sze_recovery::CanonicalEvent event = records[i].event;
        assert(writer.append(&event, records[i].payload.data()) ==
               sze_recovery::kJournalOk);
        assert(producer.publish(event, records[i].payload.data()));
    }

    std::atomic<bool> publish_tail(false);
    std::thread tail([&]() {
        while (!publish_tail.load()) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const RecoveryRecord tail_record = make_recovery_record(
            7U, 114U, 7U, make_order(114U, 7U, true, exchange_time(132)),
            exchange_time(132));
        sze_recovery::CanonicalEvent event = tail_record.event;
        assert(writer.append(&event, tail_record.payload.data()) ==
               sze_recovery::kJournalOk);
        assert(producer.publish(event, tail_record.payload.data()));
    });
    const std::vector<mix153060::StaticInputs> configured(1, inputs());
    sze_stream::SzeStreamProcessor processor(configured, 0, 1);
    sze_stream::SzeRecoveryDriver driver(&processor);
    sze_stream::SzeRecoveryJournalConfig config;
    config.directory = temporary.path;
    config.prefix = "handoff";
    config.trading_day = journal.trading_day;
    config.source_id = journal.source_id;
    config.segment_bytes = journal.segment_bytes;
    config.max_payload_bytes = journal.max_payload_bytes;
    config.shm_path = ring_path;
    config.expected_generation = writer.generation();
    config.handoff_timeout_ms = 2000U;
    publish_tail.store(true);
    assert(driver.replay_handoff(
        config, sze_stream::RecoveryTimeContext::same_boot(
            1U, static_cast<std::uint64_t>(
                exchange_us("09:30:00.000")) * 1000ULL)));
    assert(driver.live_ready());
    assert(driver.stats().records == 7U);
    assert(driver.stats().handoff_events == 1U);
    assert(producer.readiness_state() == sze_recovery::kReadinessNotReady);
    producer.publish_continuity(sze_recovery::kContinuityInvalid,
                                sze_recovery::kInvalidForwardGap,
                                7U, 114U);
    assert(!driver.poll_handoff());
    assert(!driver.available() && !driver.live_ready());
    tail.join();
    assert(writer.close(true) == sze_recovery::kJournalOk);
    producer.close();
    ::unlink(ring_path.c_str());

    TemporaryDirectory empty_directory;
    const std::string empty_ring_path = empty_directory.path + "/empty.shm";
    {
        journal.directory = empty_directory.path;
        journal.prefix = "empty";
        sze_recovery::JournalWriter empty_writer;
        assert(empty_writer.open(journal).status == sze_recovery::kJournalOk);
        assert(empty_writer.publish_continuity(sze_recovery::kContinuityValid,
                                               sze_recovery::kInvalidNone, 0U) ==
               sze_recovery::kJournalOk);
        ring_config.path = empty_ring_path;
        ring_config.generation = empty_writer.generation();
        sze_recovery::ShmEventRing empty_producer;
        assert(empty_producer.create(ring_config));
        empty_producer.publish_state(sze_recovery::kContinuityValid,
                                     sze_recovery::kReadinessNotReady,
                                     sze_recovery::kInvalidNone, 0U, 0U);
        sze_stream::SzeStreamProcessor empty_processor(configured, 0, 1);
        sze_stream::SzeRecoveryDriver empty_driver(&empty_processor);
        config.shm_path = empty_ring_path;
        config.expected_generation = empty_writer.generation();
        config.handoff_timeout_ms = 10U;
        assert(!empty_driver.replay_handoff(
            config, sze_stream::RecoveryTimeContext::analysis_exchange_time()));
        assert(!empty_driver.live_ready());
        empty_producer.close();
        assert(empty_writer.close(true) == sze_recovery::kJournalOk);
    }
    ::unlink(empty_ring_path.c_str());
}

void test_handoff_requires_epoch_and_same_boot_clock() {
    TemporaryDirectory temporary;
    sze_stream::SzeRecoveryJournalConfig config;
    config.directory = temporary.path;
    config.prefix = "missing";
    config.trading_day = 20260720U;
    config.source_id = 88U;
    config.segment_bytes = 1U << 20U;
    config.max_payload_bytes = 256U;
    config.shm_path = temporary.path + "/missing.shm";
    sze_stream::SzeStreamProcessor processor(
        std::vector<mix153060::StaticInputs>(1, inputs()), 0, 1);
    sze_stream::SzeRecoveryDriver driver(&processor);
    assert(!driver.replay_handoff(
        config, sze_stream::RecoveryTimeContext::analysis_exchange_time()));
    assert(!driver.available());

    sze_stream::SzeStreamProcessor epoch_processor(
        std::vector<mix153060::StaticInputs>(1, inputs()), 0, 1);
    sze_stream::SzeRecoveryDriver epoch_driver(&epoch_processor);
    config.expected_generation = 0U;
    assert(!epoch_driver.replay_handoff(
        config, sze_stream::RecoveryTimeContext::same_boot(
            1U, 1700000000000000000ULL)));
    assert(!epoch_driver.available());
}

void test_same_boot_clock_guards() {
    const RecoveryRecord good = make_records()[0];
    const std::vector<mix153060::StaticInputs> configured(1, inputs());
    const std::uint64_t reference_realtime = static_cast<std::uint64_t>(
        exchange_us("09:30:00.000")) * 1000ULL;
    const sze_stream::RecoveryTimeContext clock =
        sze_stream::RecoveryTimeContext::same_boot(1U, reference_realtime);
    {
        sze_stream::SzeStreamProcessor processor(configured, 0, 1);
        sze_recovery::CanonicalEvent bad = good.event;
        bool threw = false;
        try {
            processor.on_recovery(
                bad, good.payload.data(), good.payload.size(),
                sze_stream::RecoveryTimeContext::same_boot(
                    1U, std::numeric_limits<std::uint64_t>::max() - 10U));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && !processor.available());
    }
    {
        sze_stream::SzeStreamProcessor processor(configured, 0, 1);
        bool threw = false;
        try {
            processor.on_recovery(
                good.event, good.payload.data(), good.payload.size(),
                sze_stream::RecoveryTimeContext::same_boot(2000U, 10U));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && !processor.available());
    }
    {
        sze_stream::SzeStreamProcessor processor(configured, 0, 1);
        bool threw = false;
        try {
            processor.on_recovery(
                good.event, good.payload.data(), good.payload.size(),
                sze_stream::RecoveryTimeContext::same_boot(
                    1U, reference_realtime - 86400ULL * 1000000000ULL));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && !processor.available());
    }
    {
        const std::vector<RecoveryRecord> records = make_records();
        sze_stream::SzeStreamProcessor processor(configured, 0, 1);
        processor.on_recovery(records[0].event, records[0].payload.data(),
                              records[0].payload.size(), clock);
        bool threw = false;
        try {
            processor.on_recovery(records[1].event, records[1].payload.data(),
                                  records[1].payload.size(),
                                  sze_stream::RecoveryTimeContext::analysis_exchange_time());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && !processor.available());
    }
    {
        const std::vector<RecoveryRecord> records = make_records();
        sze_stream::SzeStreamProcessor processor(configured, 0, 1);
        processor.on_recovery(records[0].event, records[0].payload.data(),
                              records[0].payload.size(), clock);
        sze_recovery::CanonicalEvent regressed = records[1].event;
        regressed.receive_mono_ns = records[0].event.receive_mono_ns - 1U;
        bool threw = false;
        try {
            processor.on_recovery(regressed, records[1].payload.data(),
                                  records[1].payload.size(), clock);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && !processor.available());
    }
}

}  // namespace

int main() {
    test_journal_parity_and_filtered_gaps();
    test_invalid_recovery_is_sticky();
    test_no_implicit_eof_flush_and_clock_guard();
    test_generation_rejection();
    test_handoff_cutover_and_empty_timeout();
    test_handoff_requires_epoch_and_same_boot_clock();
    test_same_boot_clock_guards();
    std::cout << "sze_recovery_processor_test: PASS\n";
    return 0;
}
