#include "../market_data/sse_event_journal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const std::uint32_t kTradingDay = 20260907U;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string make_directory() {
    char name[] = "/tmp/sse-event-journal-XXXXXX";
    char* result = ::mkdtemp(name);
    if (!result) throw std::runtime_error("mkdtemp failed");
    return std::string(result);
}

std::string segment_path(const std::string& directory) {
    return directory + "/sse_20260907_s89_000000.szej";
}

void remove_directory(const std::string& directory) {
    const std::string first = segment_path(directory);
    (void)::unlink(first.c_str());
    for (unsigned index = 1U; index < 32U; ++index) {
        char suffix[64];
        std::snprintf(suffix, sizeof(suffix), "/sse_20260907_s89_%06u.szej", index);
        (void)::unlink((directory + suffix).c_str());
    }
    (void)::rmdir(directory.c_str());
}

std::size_t segment_count(const std::string& directory) {
    std::size_t result = 0U;
    for (unsigned index = 0U; index < 32U; ++index) {
        char suffix[64];
        std::snprintf(suffix, sizeof(suffix), "/sse_20260907_s89_%06u.szej", index);
        struct stat info;
        if (::stat((directory + suffix).c_str(), &info) == 0) ++result;
    }
    return result;
}

sse_pipeline::Event make_event(std::uint64_t event_id, std::uint8_t kind,
                               std::uint16_t channel, std::uint32_t shard,
                               unsigned char seed) {
    sse_pipeline::Event event;
    std::memset(&event, 0, sizeof(event));
    event.event_id = event_id;
    event.receive_realtime_ns = 1700000000000000000ULL + event_id;
    event.receive_mono_ns = 900000000000000000ULL + event_id;
    event.exchange_time_us = 34200000000ULL + event_id;
    event.sequence = 700000ULL + event_id;
    event.trading_day = kTradingDay;
    event.shard_id = shard;
    event.channel_no = channel;
    const bool tick_kind = kind == static_cast<std::uint8_t>(sse_pipeline::kTick) ||
                           kind == static_cast<std::uint8_t>(sse_pipeline::kTickSample);
    event.payload_size = tick_kind
        ? 72U : 440U;
    event.kind = kind;
    event.flags = static_cast<std::uint8_t>(event_id & 0xffU);
    event.version = static_cast<std::uint16_t>(sse_pipeline::kEventVersion);
    std::memcpy(event.security_id, "600001", 6U);
    for (std::size_t index = 0U; index < sizeof(event.payload); ++index) {
        event.payload[index] = static_cast<unsigned char>(seed + index);
    }
    if (tick_kind) {
        event.payload[17] = static_cast<unsigned char>(channel & 0xffU);
        event.payload[18] = static_cast<unsigned char>((channel >> 8U) & 0xffU);
    }
    return event;
}

sse_pipeline::Event make_tick_sample(const sse_pipeline::Event& previous) {
    sse_pipeline::Event event = previous;
    event.event_id = previous.event_id + 1U;
    event.kind = static_cast<std::uint8_t>(sse_pipeline::kTickSample);
    event.receive_realtime_ns = previous.receive_realtime_ns;
    event.receive_mono_ns = previous.receive_mono_ns + 101000ULL;
    event.sequence = previous.sequence;
    return event;
}

void test_roundtrip_and_rollover() {
    const std::string directory = make_directory();
    try {
        sse_pipeline::JournalOptions options;
        options.directory = directory;
        options.trading_day = kTradingDay;
        options.segment_bytes = 8192U;
        options.min_free_bytes = 0U;

        sse_pipeline::EventJournal writer;
        std::string error;
        check(writer.open(options, &error), "writer open");
        std::vector<sse_pipeline::Event> expected;
        for (std::uint64_t id = 1U; id <= 20U; ++id) {
            const bool tick = (id % 3U) != 0U;
            const std::uint16_t channel = tick
                ? static_cast<std::uint16_t>((id - 1U) % 6U + 1U) : 0U;
            const std::uint32_t shard = tick
                ? static_cast<std::uint32_t>((id - 1U) % 3U)
                : sse_pipeline::kUnassignedShard;
            expected.push_back(make_event(
                id, tick ? static_cast<std::uint8_t>(sse_pipeline::kTick)
                         : static_cast<std::uint8_t>(sse_pipeline::kSnapshot),
                channel, shard, static_cast<unsigned char>(id)));
            check(writer.append(expected.back(), &error), "append event");
        }
        expected.push_back(make_tick_sample(expected[19U]));
        check(writer.append(expected.back(), &error), "append tick sample marker");
        check(writer.flush(&error), "flush journal");
        check(writer.close(true, &error), "close journal cleanly");
        check(segment_count(directory) > 1U, "journal must roll over");

        sse_pipeline::EventJournal duplicate_writer;
        check(!duplicate_writer.open(options, &error),
              "existing journal must be rejected by a new writer");

        sse_pipeline::EventJournalReader reader;
        check(reader.open(options, &error), "reader open");
        for (std::size_t index = 0U; index < expected.size(); ++index) {
            sse_pipeline::Event actual;
            check(reader.next(&actual, &error) ==
                  sse_pipeline::EventJournalReader::Record,
                  "reader record");
            check(std::memcmp(&actual, &expected[index], sizeof(actual)) == 0,
                  "round-trip event mismatch");
        }
        sse_pipeline::Event end;
        check(reader.next(&end, &error) == sse_pipeline::EventJournalReader::End,
              "reader end");
        check(reader.close(), "reader close");
    } catch (...) {
        remove_directory(directory);
        throw;
    }
    remove_directory(directory);
}

void test_invalid_events() {
    const std::string directory = make_directory();
    try {
        sse_pipeline::JournalOptions options;
        options.directory = directory;
        options.trading_day = kTradingDay;
        options.segment_bytes = 8192U;
        options.min_free_bytes = 0U;
        sse_pipeline::EventJournal writer;
        std::string error;
        check(writer.open(options, &error), "invalid test writer open");

        sse_pipeline::Event event = make_event(1U, sse_pipeline::kTick, 1U, 0U, 1U);
        event.version = 2U;
        check(!writer.append(event, &error), "invalid version accepted");
        event = make_event(1U, sse_pipeline::kTick, 0U, 0U, 2U);
        check(!writer.append(event, &error), "unassigned tick accepted");
        event = make_event(1U, sse_pipeline::kSnapshot, 0U, 0U, 3U);
        event.payload_size = 72U;
        check(!writer.append(event, &error), "invalid snapshot size accepted");
        event = make_event(1U, sse_pipeline::kTick, 1U, 0U, 4U);
        event.payload[17] = 2U;
        check(!writer.append(event, &error), "mismatched tick channel accepted");
        event = make_event(1U, sse_pipeline::kTick, 1U, 0U, 5U);
        event.security_id[0] = 'X';
        check(!writer.append(event, &error), "invalid security id accepted");
        event = make_event(1U, sse_pipeline::kTick, 1U, 0U, 6U);
        event.event_id = 2U;
        check(!writer.append(event, &error), "event id gap accepted");
        check(writer.close(false, &error), "invalid test close");
    } catch (...) {
        remove_directory(directory);
        throw;
    }
    remove_directory(directory);
}

void test_corruption_and_truncation() {
    const std::string directory = make_directory();
    try {
        sse_pipeline::JournalOptions options;
        options.directory = directory;
        options.trading_day = kTradingDay;
        options.segment_bytes = 8192U;
        options.min_free_bytes = 0U;
        std::string error;

        sse_pipeline::EventJournal writer;
        check(writer.open(options, &error), "corruption writer open");
        const sse_pipeline::Event event = make_event(
            1U, sse_pipeline::kTick, 1U, 0U, 9U);
        check(writer.append(event, &error), "corruption append");
        check(writer.close(true, &error), "corruption clean close");

        const std::string path = segment_path(directory);
        const int fd = ::open(path.c_str(), O_RDWR);
        check(fd >= 0, "open segment for corruption");
        const unsigned char changed = 0xffU;
        check(::pwrite(fd, &changed, 1U, 4096 + sizeof(sze_recovery::JournalRecordHeader)) == 1,
              "corrupt record payload");
        ::close(fd);

        sse_pipeline::EventJournalReader reader;
        check(reader.open(options, &error), "corrupt reader open");
        sse_pipeline::Event output;
        check(reader.next(&output, &error) ==
              sse_pipeline::EventJournalReader::Error,
              "corrupt payload accepted");
        reader.close();

        check(::truncate(path.c_str(), 4096 + 12) == 0, "truncate segment");
        sse_pipeline::EventJournalReader truncated_reader;
        check(!truncated_reader.open(options, &error),
              "truncated segment accepted");
    } catch (...) {
        remove_directory(directory);
        throw;
    }
    remove_directory(directory);
}

}  // namespace

int main() {
    try {
        test_roundtrip_and_rollover();
        test_invalid_events();
        test_corruption_and_truncation();
        std::cout << "sse_event_journal_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
