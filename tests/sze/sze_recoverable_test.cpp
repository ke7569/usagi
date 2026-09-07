#include "sze/market_data/SZERecoverable.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::string make_temp_directory()
{
    char path[] = "/tmp/sze_recovery_test_XXXXXX";
    char* result = ::mkdtemp(path);
    assert(result != 0);
    return result;
}

sze_recovery::CanonicalEvent make_event(std::uint64_t feed_sequence,
                                        std::uint8_t message_type,
                                        std::size_t payload_size)
{
    sze_recovery::CanonicalEvent event;
    std::memset(&event, 0, sizeof(event));
    event.feed_sequence = feed_sequence;
    event.channel_sequence = feed_sequence + 1000U;
    event.receive_mono_ns = feed_sequence * 100U;
    event.exchange_time = 20260721100000000ULL + feed_sequence;
    event.channel_number = 2011U;
    event.payload_size = static_cast<std::uint16_t>(payload_size);
    event.message_type = message_type;
    event.record_kind = sze_recovery::kRecordMarketData;
    return event;
}

void test_sequence_tracker()
{
    sze_recovery::FeedSequenceTracker tracker;
    tracker.reset(20260721U);
    sze_recovery::SequenceResult result = tracker.observe(100U);
    assert(result.status == sze_recovery::kSequenceFirst);
    assert(tracker.state() == sze_recovery::kContinuityValid);
    result = tracker.observe(101U);
    assert(result.status == sze_recovery::kSequenceAccepted);
    result = tracker.observe(101U);
    assert(result.status == sze_recovery::kSequenceDuplicate);
    assert(tracker.duplicate_count() == 1U);
    result = tracker.observe(103U);
    assert(result.status == sze_recovery::kSequenceGap);
    assert(result.expected == 102U);
    assert(result.missing == 1U);
    assert(tracker.state() == sze_recovery::kContinuityInvalid);
    assert(tracker.invalid_reason() == sze_recovery::kInvalidForwardGap);
    assert(tracker.observe(102U).status == sze_recovery::kSequenceAlreadyInvalid);

    tracker.reset(20260722U);
    assert(tracker.state() == sze_recovery::kContinuityInitializing);
    assert(tracker.observe(std::numeric_limits<std::uint32_t>::max() - 1U).status ==
           sze_recovery::kSequenceFirst);
    assert(tracker.observe(std::numeric_limits<std::uint32_t>::max()).status ==
           sze_recovery::kSequenceAccepted);
    result = tracker.observe(0U);
    assert(result.status == sze_recovery::kSequenceAccepted);
    assert(result.sequence == (1ULL << 32U));
    result = tracker.observe(1U);
    assert(result.sequence == (1ULL << 32U) + 1U);

    tracker.reset(20260723U);
    assert(tracker.observe(500U).status == sze_recovery::kSequenceFirst);
    result = tracker.observe(499U);
    assert(result.status == sze_recovery::kSequenceRegression);
    assert(tracker.invalid_reason() == sze_recovery::kInvalidRegression);
    tracker.invalidate(sze_recovery::kInvalidMalformedRecord);
    assert(tracker.invalid_reason() == sze_recovery::kInvalidRegression);
}

void remove_journal(const sze_recovery::JournalConfig& config,
                    std::uint32_t max_segments)
{
    sze_recovery::JournalWriter naming;
    sze_recovery::JournalConfig mutable_config = config;
    // segment_path() needs an opened writer config, so reproduce the stable ABI
    // file name used by the component for test cleanup.
    for (std::uint32_t index = 0; index < max_segments; ++index) {
        char suffix[128];
        std::snprintf(suffix, sizeof(suffix), "%s/%s_%08u_s%u_%06u.szej",
                      config.directory.c_str(), config.prefix.c_str(),
                      config.trading_day, config.source_id, index);
        ::unlink(suffix);
    }
    (void)naming;
    (void)mutable_config;
}

void test_journal_disk_reserve(const std::string& directory)
{
    sze_recovery::JournalConfig config;
    config.directory = directory;
    config.prefix = "disk_reserve";
    config.trading_day = 20260721U;
    config.source_id = 88U;
    config.segment_bytes = 8192U;
    config.max_payload_bytes = 128U;
    config.min_free_bytes_after_allocate =
        std::numeric_limits<std::uint64_t>::max();

    sze_recovery::JournalWriter writer;
    const sze_recovery::JournalOpenResult opened = writer.open(config);
    assert(opened.status == sze_recovery::kJournalIoError);
    assert(!writer.is_open());
}

void test_journal_clean_reopen_and_rotation(const std::string& directory)
{
    sze_recovery::JournalConfig config;
    config.directory = directory;
    config.prefix = "clean";
    config.trading_day = 20260721U;
    config.source_id = 88U;
    config.segment_bytes = 4576U;  // Three 160-byte records per segment.
    config.max_payload_bytes = 128U;
    config.generation = 7U;

    unsigned char payload[72];
    std::memset(payload, 0x5a, sizeof(payload));
    sze_recovery::JournalWriter writer;
    sze_recovery::JournalOpenResult opened = writer.open(config);
    assert(opened.status == sze_recovery::kJournalOk);
    assert(!opened.existing);
    assert(writer.publish_continuity(sze_recovery::kContinuityValid,
                                     sze_recovery::kInvalidNone, 99U) ==
           sze_recovery::kJournalOk);
    for (std::uint64_t sequence = 100U; sequence < 105U; ++sequence) {
        sze_recovery::CanonicalEvent event = make_event(sequence, 23U, sizeof(payload));
        assert(writer.append(&event, payload) == sze_recovery::kJournalOk);
        assert(event.event_id == sequence - 99U);
    }
    assert(writer.segment_index() == 1U);
    assert(writer.flush(false) == sze_recovery::kJournalOk);
    assert(writer.flush_count() >= 2U);  // Rotation performs a synchronous flush.

    sze_recovery::JournalReader live_reader;
    assert(live_reader.open(config).status == sze_recovery::kJournalOk);
    for (std::uint64_t expected = 1U; expected <= 5U; ++expected) {
        sze_recovery::CanonicalEvent event;
        unsigned char output[72];
        assert(live_reader.next(&event, output, sizeof(output)) ==
               sze_recovery::kJournalOk);
        assert(event.event_id == expected);
        assert(event.feed_sequence == expected + 99U);
        assert(std::memcmp(output, payload, sizeof(output)) == 0);
    }
    sze_recovery::CanonicalEvent live_end_event;
    assert(live_reader.next(&live_end_event, payload, sizeof(payload)) ==
           sze_recovery::kJournalWouldBlock);
    sze_recovery::CanonicalEvent live_sixth = make_event(
        105U, 24U, sizeof(payload));
    assert(writer.append(&live_sixth, payload) == sze_recovery::kJournalOk);
    assert(live_sixth.event_id == 6U);
    unsigned char live_output[72];
    assert(live_reader.next(&live_end_event, live_output, sizeof(live_output)) ==
           sze_recovery::kJournalOk);
    assert(live_end_event.event_id == 6U);
    live_reader.close();

    assert(writer.close(true) == sze_recovery::kJournalOk);
    opened = writer.open(config);
    assert(opened.status == sze_recovery::kJournalOk);
    assert(opened.existing);
    assert(!opened.unclean_restart);
    assert(!opened.corrupt_tail);
    assert(opened.last_event_id == 6U);
    sze_recovery::CanonicalEvent seventh = make_event(106U, 24U, sizeof(payload));
    assert(writer.append(&seventh, payload) == sze_recovery::kJournalOk);
    assert(seventh.event_id == 7U);
    assert(writer.close(true) == sze_recovery::kJournalOk);

    sze_recovery::JournalReader reader;
    assert(reader.open(config).status == sze_recovery::kJournalOk);
    for (std::uint64_t expected = 1U; expected <= 7U; ++expected) {
        sze_recovery::CanonicalEvent event;
        unsigned char output[72];
        assert(reader.next(&event, output, sizeof(output)) == sze_recovery::kJournalOk);
        assert(event.event_id == expected);
    }
    sze_recovery::CanonicalEvent end_event;
    assert(reader.next(&end_event, payload, sizeof(payload)) == sze_recovery::kJournalEnd);
    reader.close();
    remove_journal(config, 4U);
}

void corrupt_last_commit(const std::string& path, std::uint64_t segment_bytes)
{
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    void* memory = ::mmap(0, static_cast<std::size_t>(segment_bytes),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(memory != MAP_FAILED);
    unsigned char* bytes = static_cast<unsigned char*>(memory);
    sze_recovery::JournalSuperblock* block =
        reinterpret_cast<sze_recovery::JournalSuperblock*>(bytes);
    const std::uint64_t published = block->published_offset;
    assert(published > sze_recovery::kPageBytes);
    sze_recovery::JournalRecordHeader header;
    std::memcpy(&header, bytes + sze_recovery::kPageBytes, sizeof(header));
    sze_recovery::JournalRecordTrailer* trailer =
        reinterpret_cast<sze_recovery::JournalRecordTrailer*>(
            bytes + sze_recovery::kPageBytes + header.total_bytes -
            sizeof(sze_recovery::JournalRecordTrailer));
    trailer->commit_magic = 0U;
    assert(::msync(memory, static_cast<std::size_t>(published), MS_SYNC) == 0);
    ::munmap(memory, static_cast<std::size_t>(segment_bytes));
    ::close(fd);
}

void corrupt_first_payload(const std::string& path, std::uint64_t segment_bytes)
{
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    void* memory = ::mmap(0, static_cast<std::size_t>(segment_bytes),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(memory != MAP_FAILED);
    unsigned char* bytes = static_cast<unsigned char*>(memory);
    sze_recovery::JournalRecordHeader header;
    std::memcpy(&header, bytes + sze_recovery::kPageBytes, sizeof(header));
    assert(header.payload_bytes > 0U);
    bytes[sze_recovery::kPageBytes + sizeof(header)] ^= 0xffU;
    assert(::msync(memory, static_cast<std::size_t>(
        sze_recovery::kPageBytes + header.total_bytes), MS_SYNC) == 0);
    ::munmap(memory, static_cast<std::size_t>(segment_bytes));
    ::close(fd);
}

void test_journal_corrupt_tail_and_unclean_restart(const std::string& directory)
{
    sze_recovery::JournalConfig config;
    config.directory = directory;
    config.prefix = "corrupt";
    config.trading_day = 20260721U;
    config.source_id = 88U;
    config.segment_bytes = 8192U;
    config.max_payload_bytes = 128U;

    unsigned char payload[72];
    std::memset(payload, 0x3c, sizeof(payload));
    {
        sze_recovery::JournalWriter writer;
        assert(writer.open(config).status == sze_recovery::kJournalOk);
        sze_recovery::CanonicalEvent event = make_event(700U, 23U, sizeof(payload));
        assert(writer.append(&event, payload) == sze_recovery::kJournalOk);
        assert(writer.flush(true) == sze_recovery::kJournalOk);
        // Destruction deliberately leaves clean_shutdown=0.
    }
    char path[128];
    std::snprintf(path, sizeof(path), "%s/%s_%08u_s%u_%06u.szej",
                  config.directory.c_str(), config.prefix.c_str(),
                  config.trading_day, config.source_id, 0U);
    corrupt_last_commit(path, config.segment_bytes);

    sze_recovery::JournalWriter recovered;
    const sze_recovery::JournalOpenResult result = recovered.open(config);
    assert(result.status == sze_recovery::kJournalOk);
    assert(result.existing);
    assert(result.unclean_restart);
    assert(result.corrupt_tail);
    assert(result.last_event_id == 0U);
    sze_recovery::CanonicalEvent replacement = make_event(701U, 24U, sizeof(payload));
    assert(recovered.append(&replacement, payload) == sze_recovery::kJournalOk);
    assert(replacement.event_id == 1U);
    assert(recovered.close(true) == sze_recovery::kJournalOk);
    remove_journal(config, 2U);
}

void test_journal_checksum_corruption(const std::string& directory)
{
    sze_recovery::JournalConfig config;
    config.directory = directory;
    config.prefix = "checksum";
    config.trading_day = 20260721U;
    config.source_id = 88U;
    config.segment_bytes = 8192U;
    config.max_payload_bytes = 128U;

    unsigned char payload[72];
    std::memset(payload, 0x69, sizeof(payload));
    sze_recovery::JournalWriter writer;
    assert(writer.open(config).status == sze_recovery::kJournalOk);
    sze_recovery::CanonicalEvent event = make_event(800U, 23U, sizeof(payload));
    assert(writer.append(&event, payload) == sze_recovery::kJournalOk);
    assert(writer.close(true) == sze_recovery::kJournalOk);

    char path[128];
    std::snprintf(path, sizeof(path), "%s/%s_%08u_s%u_%06u.szej",
                  config.directory.c_str(), config.prefix.c_str(),
                  config.trading_day, config.source_id, 0U);
    corrupt_first_payload(path, config.segment_bytes);

    const sze_recovery::JournalOpenResult reopened = writer.open(config);
    assert(reopened.status == sze_recovery::kJournalOk);
    assert(!reopened.unclean_restart);
    assert(reopened.corrupt_tail);
    assert(reopened.last_event_id == 0U);
    assert(writer.close(true) == sze_recovery::kJournalOk);
    remove_journal(config, 2U);
}

void test_shm_ring(const std::string& directory)
{
    const std::string path = directory + "/events.shm";
    sze_recovery::RingConfig config;
    config.path = path;
    config.trading_day = 20260721U;
    config.source_id = 88U;
    config.capacity = 4U;
    config.max_payload_bytes = 128U;
    config.generation = 12345U;

    sze_recovery::ShmEventRing producer;
    assert(producer.create(config));
    sze_recovery::ShmEventRing consumer;
    assert(consumer.attach(path));
    assert(consumer.generation() == 12345U);

    unsigned char payload[32];
    std::memset(payload, 0x7b, sizeof(payload));
    for (std::uint64_t id = 1U; id <= 6U; ++id) {
        sze_recovery::CanonicalEvent event = make_event(100U + id, 23U, sizeof(payload));
        event.event_id = id;
        event.payload_crc32 = sze_recovery::crc32(payload, sizeof(payload));
        assert(producer.publish(event, payload));
        if (id == 1U) {
            sze_recovery::CanonicalEvent output;
            unsigned char output_payload[32];
            assert(consumer.read(1U, &output, output_payload, sizeof(output_payload)) ==
                   sze_recovery::kRingReadOk);
            assert(output.event_id == 1U);
            assert(std::memcmp(output_payload, payload, sizeof(payload)) == 0);
        }
    }
    sze_recovery::CanonicalEvent output;
    unsigned char output_payload[32];
    assert(consumer.read(1U, &output, output_payload, sizeof(output_payload)) ==
           sze_recovery::kRingReadOverrun);
    assert(consumer.read(3U, &output, output_payload, sizeof(output_payload)) ==
           sze_recovery::kRingReadOk);
    assert(consumer.read(7U, &output, output_payload, sizeof(output_payload)) ==
           sze_recovery::kRingReadNotReady);

    producer.publish_state(sze_recovery::kContinuityValid,
                           sze_recovery::kReadinessLiveReady,
                           sze_recovery::kInvalidNone, 0U, 106U);
    assert(consumer.continuity_state() == sze_recovery::kContinuityValid);
    assert(consumer.readiness_state() == sze_recovery::kReadinessLiveReady);
    producer.publish_capture_metrics(100U, 6U, 2U, 1U, 3U);
    producer.publish_storage_metrics(6U, 0U, 4U, 8192U, 8000U);
    consumer.publish_replay_metrics(5U, 1U, 300000U, 2U, 1U, 500U);
    assert(producer.header()->capture_records == 100U);
    assert(producer.header()->selected_records == 6U);
    assert(producer.header()->journal_published_offset == 8192U);
    assert(producer.header()->replay_event_id == 5U);
    assert(producer.header()->replay_lag == 1U);
    assert(producer.header()->replay_rate_milli == 300000U);
    producer.publish_state(sze_recovery::kContinuityInvalid,
                           sze_recovery::kReadinessLiveReady,
                           sze_recovery::kInvalidForwardGap, 6U, 106U);
    assert(consumer.continuity_state() == sze_recovery::kContinuityInvalid);
    assert(consumer.readiness_state() == sze_recovery::kReadinessNotReady);
    producer.publish_state(sze_recovery::kContinuityValid,
                           sze_recovery::kReadinessLiveReady,
                           sze_recovery::kInvalidNone, 0U, 107U);
    assert(consumer.continuity_state() == sze_recovery::kContinuityInvalid);
    assert(consumer.readiness_state() == sze_recovery::kReadinessNotReady);

    sze_recovery::ShmRingHeader* header =
        const_cast<sze_recovery::ShmRingHeader*>(producer.header());
    ++header->generation;
    assert(consumer.read(6U, &output, output_payload, sizeof(output_payload)) ==
           sze_recovery::kRingReadStaleGeneration);

    consumer.close();
    producer.close();
    ::unlink(path.c_str());
}

void test_shm_concurrent_publication(const std::string& directory)
{
    const std::string path = directory + "/concurrent.shm";
    sze_recovery::RingConfig config;
    config.path = path;
    config.trading_day = 20260721U;
    config.capacity = 16384U;
    config.max_payload_bytes = 64U;

    sze_recovery::ShmEventRing producer;
    sze_recovery::ShmEventRing consumer;
    assert(producer.create(config));
    assert(consumer.attach(path));
    const std::uint64_t count = 10000U;
    std::thread publisher([&producer, count]() {
        for (std::uint64_t id = 1U; id <= count; ++id) {
            const std::uint64_t payload = id ^ 0xa5a5a5a5ULL;
            sze_recovery::CanonicalEvent event = make_event(id, 24U, sizeof(payload));
            event.event_id = id;
            event.payload_crc32 = sze_recovery::crc32(&payload, sizeof(payload));
            assert(producer.publish(event, &payload));
        }
    });
    for (std::uint64_t expected = 1U; expected <= count; ++expected) {
        for (;;) {
            sze_recovery::CanonicalEvent event;
            std::uint64_t payload = 0U;
            const sze_recovery::RingReadStatus status = consumer.read(
                expected, &event, &payload, sizeof(payload));
            if (status == sze_recovery::kRingReadNotReady) {
                std::this_thread::yield();
                continue;
            }
            assert(status == sze_recovery::kRingReadOk);
            assert(event.event_id == expected);
            assert(payload == (expected ^ 0xa5a5a5a5ULL));
            break;
        }
    }
    publisher.join();
    consumer.close();
    producer.close();
    ::unlink(path.c_str());
}

void test_replay_handoff_and_overrun_fallback(const std::string& directory)
{
    sze_recovery::JournalConfig journal_config;
    journal_config.directory = directory;
    journal_config.prefix = "handoff";
    journal_config.trading_day = 20260721U;
    journal_config.source_id = 88U;
    journal_config.segment_bytes = 8192U;
    journal_config.max_payload_bytes = 64U;

    sze_recovery::JournalWriter writer;
    assert(writer.open(journal_config).status == sze_recovery::kJournalOk);
    assert(writer.publish_continuity(sze_recovery::kContinuityValid,
                                     sze_recovery::kInvalidNone, 0U) ==
           sze_recovery::kJournalOk);

    const std::string ring_path = directory + "/handoff.shm";
    sze_recovery::RingConfig ring_config;
    ring_config.path = ring_path;
    ring_config.trading_day = journal_config.trading_day;
    ring_config.source_id = journal_config.source_id;
    ring_config.capacity = 4U;
    ring_config.max_payload_bytes = 64U;
    ring_config.generation = writer.generation();
    sze_recovery::ShmEventRing producer;
    assert(producer.create(ring_config));
    producer.publish_state(sze_recovery::kContinuityValid,
                           sze_recovery::kReadinessNotReady,
                           sze_recovery::kInvalidNone, 0U, 0U);

    for (std::uint64_t id = 1U; id <= 3U; ++id) {
        const std::uint64_t payload = id * 10U;
        sze_recovery::CanonicalEvent event = make_event(id, 23U, sizeof(payload));
        assert(writer.append(&event, &payload) == sze_recovery::kJournalOk);
        assert(event.event_id == id);
        assert(producer.publish(event, &payload));
    }

    sze_recovery::ReplayHandoffConsumer consumer;
    assert(consumer.open(journal_config, ring_path));
    assert(consumer.mode() == sze_recovery::kReplayJournal);
    for (std::uint64_t id = 1U; id <= 3U; ++id) {
        sze_recovery::CanonicalEvent event;
        std::uint64_t payload = 0U;
        assert(consumer.next(&event, &payload, sizeof(payload)) ==
               sze_recovery::kReplayReadEvent);
        assert(event.event_id == id);
        assert(payload == id * 10U);
    }
    sze_recovery::CanonicalEvent event;
    std::uint64_t payload = 0U;
    assert(consumer.next(&event, &payload, sizeof(payload)) ==
           sze_recovery::kReplayReadWouldBlock);
    assert(consumer.mode() == sze_recovery::kReplayHandoff);
    assert(producer.readiness_state() == sze_recovery::kReadinessHandoff);

    payload = 40U;
    event = make_event(4U, 24U, sizeof(payload));
    assert(writer.append(&event, &payload) == sze_recovery::kJournalOk);
    assert(producer.publish(event, &payload));
    payload = 0U;
    assert(consumer.next(&event, &payload, sizeof(payload)) ==
           sze_recovery::kReplayReadEvent);
    assert(event.event_id == 4U && payload == 40U);
    assert(consumer.mode() == sze_recovery::kReplayLive);
    assert(producer.readiness_state() == sze_recovery::kReadinessLiveReady);
    producer.publish_continuity(sze_recovery::kContinuityValid,
                                sze_recovery::kInvalidNone, 0U, 4U);
    assert(producer.readiness_state() == sze_recovery::kReadinessLiveReady);

    for (std::uint64_t id = 5U; id <= 9U; ++id) {
        payload = id * 10U;
        event = make_event(id, 23U, sizeof(payload));
        assert(writer.append(&event, &payload) == sze_recovery::kJournalOk);
        assert(producer.publish(event, &payload));
    }
    payload = 0U;
    assert(consumer.next(&event, &payload, sizeof(payload)) ==
           sze_recovery::kReplayReadWouldBlock);
    assert(consumer.mode() == sze_recovery::kReplayJournal);
    assert(consumer.ring_overruns() == 1U);
    assert(consumer.handoff_retries() == 1U);

    for (std::uint64_t id = 5U; id <= 9U; ++id) {
        payload = 0U;
        assert(consumer.next(&event, &payload, sizeof(payload)) ==
               sze_recovery::kReplayReadEvent);
        assert(event.event_id == id && payload == id * 10U);
    }
    assert(consumer.next(&event, &payload, sizeof(payload)) ==
           sze_recovery::kReplayReadWouldBlock);
    assert(consumer.mode() == sze_recovery::kReplayHandoff);

    payload = 100U;
    event = make_event(10U, 24U, sizeof(payload));
    assert(writer.append(&event, &payload) == sze_recovery::kJournalOk);
    assert(producer.publish(event, &payload));
    payload = 0U;
    assert(consumer.next(&event, &payload, sizeof(payload)) ==
           sze_recovery::kReplayReadEvent);
    assert(event.event_id == 10U && payload == 100U);
    assert(consumer.mode() == sze_recovery::kReplayLive);

    producer.publish_continuity(sze_recovery::kContinuityInvalid,
                                sze_recovery::kInvalidForwardGap, 10U, 10U);
    assert(consumer.next(&event, &payload, sizeof(payload)) ==
           sze_recovery::kReplayReadInvalid);
    assert(consumer.mode() == sze_recovery::kReplayInvalid);
    assert(producer.readiness_state() == sze_recovery::kReadinessNotReady);
    consumer.close();
    assert(writer.close(true) == sze_recovery::kJournalOk);
    producer.close();
    ::unlink(ring_path.c_str());
    remove_journal(journal_config, 2U);
}

void test_long_restart_concurrent_capture_and_no_timeout(
    const std::string& directory)
{
    sze_recovery::JournalConfig journal_config;
    journal_config.directory = directory;
    journal_config.prefix = "long_restart";
    journal_config.trading_day = 20260721U;
    journal_config.source_id = 88U;
    journal_config.segment_bytes = 1U << 20U;
    journal_config.max_payload_bytes = 64U;

    sze_recovery::JournalWriter writer;
    assert(writer.open(journal_config).status == sze_recovery::kJournalOk);
    assert(writer.publish_continuity(sze_recovery::kContinuityValid,
                                     sze_recovery::kInvalidNone, 0U) ==
           sze_recovery::kJournalOk);

    const std::string ring_path = directory + "/long_restart.shm";
    sze_recovery::RingConfig ring_config;
    ring_config.path = ring_path;
    ring_config.trading_day = journal_config.trading_day;
    ring_config.source_id = journal_config.source_id;
    ring_config.capacity = 4096U;
    ring_config.max_payload_bytes = 64U;
    ring_config.generation = writer.generation();
    sze_recovery::ShmEventRing producer;
    assert(producer.create(ring_config));
    producer.publish_state(sze_recovery::kContinuityValid,
                           sze_recovery::kReadinessNotReady,
                           sze_recovery::kInvalidNone, 0U, 0U);

    const std::uint64_t offline_events = 20000U;
    const std::uint64_t concurrent_events = 30000U;
    for (std::uint64_t id = 1U; id <= offline_events; ++id) {
        const std::uint64_t payload = id * 17U;
        sze_recovery::CanonicalEvent event = make_event(
            id, id % 2U ? 23U : 24U, sizeof(payload));
        event.receive_mono_ns = id * 90000000ULL;  // Thirty minutes of feed time.
        assert(writer.append(&event, &payload) == sze_recovery::kJournalOk);
        assert(producer.publish(event, &payload));
    }

    sze_recovery::ReplayHandoffConsumer consumer;
    assert(consumer.open(journal_config, ring_path));
    std::thread publisher([&writer, &producer, offline_events,
                           concurrent_events]() {
        for (std::uint64_t id = offline_events + 1U;
             id <= concurrent_events; ++id) {
            const std::uint64_t payload = id * 17U;
            sze_recovery::CanonicalEvent event = make_event(
                id, id % 2U ? 23U : 24U, sizeof(payload));
            event.receive_mono_ns = id * 90000000ULL;
            assert(writer.append(&event, &payload) == sze_recovery::kJournalOk);
            assert(producer.publish(event, &payload));
        }
    });

    std::uint64_t expected = 1U;
    while (expected <= concurrent_events) {
        sze_recovery::CanonicalEvent event;
        std::uint64_t payload = 0U;
        const sze_recovery::ReplayReadStatus status = consumer.next(
            &event, &payload, sizeof(payload));
        if (status == sze_recovery::kReplayReadWouldBlock) {
            std::this_thread::yield();
            continue;
        }
        assert(status == sze_recovery::kReplayReadEvent);
        assert(event.event_id == expected);
        assert(payload == expected * 17U);
        ++expected;
    }
    publisher.join();

    sze_recovery::CanonicalEvent event;
    std::uint64_t payload = 0U;
    assert(consumer.next(&event, &payload, sizeof(payload)) ==
           sze_recovery::kReplayReadWouldBlock);
    assert(consumer.mode() == sze_recovery::kReplayHandoff);
    consumer.publish_metrics(1000U, 600001U);
    assert(producer.header()->recovery_elapsed_ms == 600001U);

    payload = (concurrent_events + 1U) * 17U;
    event = make_event(concurrent_events + 1U, 23U, sizeof(payload));
    assert(writer.append(&event, &payload) == sze_recovery::kJournalOk);
    assert(producer.publish(event, &payload));
    payload = 0U;
    assert(consumer.next(&event, &payload, sizeof(payload)) ==
           sze_recovery::kReplayReadEvent);
    assert(event.event_id == concurrent_events + 1U);
    assert(payload == (concurrent_events + 1U) * 17U);
    assert(consumer.mode() == sze_recovery::kReplayLive);
    assert(producer.readiness_state() == sze_recovery::kReadinessLiveReady);

    consumer.close();
    assert(writer.close(true) == sze_recovery::kJournalOk);
    producer.close();
    ::unlink(ring_path.c_str());
    remove_journal(journal_config, 64U);
}

void test_invalid_journal_analysis_reader(const std::string& directory)
{
    sze_recovery::JournalConfig config;
    config.directory = directory;
    config.prefix = "invalid_analysis";
    config.trading_day = 20260724U;
    config.source_id = 88U;
    config.segment_bytes = 1U << 20U;
    config.max_payload_bytes = 128U;

    const std::uint64_t payload = 0x1122334455667788ULL;
    sze_recovery::JournalWriter writer;
    assert(writer.open(config).status == sze_recovery::kJournalOk);
    for (std::uint64_t id = 1U; id <= 4U; ++id) {
        sze_recovery::CanonicalEvent event = make_event(
            id, id % 2U == 0U ? 24U : 23U, sizeof(payload));
        assert(writer.append(&event, &payload) == sze_recovery::kJournalOk);
    }
    assert(writer.publish_continuity(
               sze_recovery::kContinuityInvalid,
               sze_recovery::kInvalidUncleanRestart, 4U) ==
           sze_recovery::kJournalOk);
    assert(writer.close(true) == sze_recovery::kJournalOk);

    sze_recovery::JournalReader reader;
    const sze_recovery::JournalOpenResult opened = reader.open(config);
    assert(opened.status == sze_recovery::kJournalOk);
    assert(opened.continuity_state == sze_recovery::kContinuityInvalid);
    assert(opened.invalid_reason == sze_recovery::kInvalidUncleanRestart);
    for (std::uint64_t id = 1U; id <= 4U; ++id) {
        sze_recovery::CanonicalEvent event;
        std::uint64_t output = 0U;
        assert(reader.next(&event, &output, sizeof(output)) ==
               sze_recovery::kJournalOk);
        assert(event.event_id == id);
        assert(output == payload);
    }
    sze_recovery::CanonicalEvent end;
    std::uint64_t end_payload = 0U;
    assert(reader.next(&end, &end_payload, sizeof(end_payload)) ==
           sze_recovery::kJournalEnd);
    reader.close();
    remove_journal(config, 4U);
}

}  // namespace

int main()
{
    test_sequence_tracker();
    const std::string directory = make_temp_directory();
    test_journal_disk_reserve(directory);
    test_journal_clean_reopen_and_rotation(directory);
    test_journal_corrupt_tail_and_unclean_restart(directory);
    test_journal_checksum_corruption(directory);
    test_shm_ring(directory);
    test_shm_concurrent_publication(directory);
    test_replay_handoff_and_overrun_fallback(directory);
    test_long_restart_concurrent_capture_and_no_timeout(directory);
    test_invalid_journal_analysis_reader(directory);
    assert(::rmdir(directory.c_str()) == 0);
    return 0;
}
