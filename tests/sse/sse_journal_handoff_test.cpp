#include "common/recovery/SZERecoverable.h"
#include "sse/runtime/sse_shm_prefetch.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string temporary_directory() {
    char path[] = "/tmp/sse_journal_handoff_XXXXXX";
    char* result = ::mkdtemp(path);
    check(result != 0, "cannot create handoff test directory");
    return result;
}

std::string segment_path(const sze_recovery::JournalConfig& config,
                         std::uint32_t index) {
    char path[512];
    const int written = std::snprintf(
        path, sizeof(path), "%s/%s_%08u_s%u_%06u.szej",
        config.directory.c_str(), config.prefix.c_str(), config.trading_day,
        config.source_id, index);
    check(written > 0 && static_cast<std::size_t>(written) < sizeof(path),
          "journal segment path overflow");
    return path;
}

void remove_journal(const sze_recovery::JournalConfig& config,
                    std::uint32_t segment_count) {
    for (std::uint32_t index = 0; index < segment_count; ++index)
        (void)::unlink(segment_path(config, index).c_str());
}

sze_recovery::CanonicalEvent make_event(std::uint64_t feed_sequence,
                                        std::uint8_t message_type) {
    sze_recovery::CanonicalEvent event;
    std::memset(&event, 0, sizeof(event));
    event.feed_sequence = feed_sequence;
    event.channel_sequence = feed_sequence + 1000U;
    event.receive_mono_ns = feed_sequence * 1000000U;
    event.exchange_time = 93000000000ULL + feed_sequence;
    event.channel_number = 1U;
    event.payload_size = sizeof(std::uint64_t);
    event.message_type = message_type;
    event.record_kind = sze_recovery::kRecordMarketData;
    return event;
}

void append_and_publish(sze_recovery::JournalWriter* writer,
                        sze_recovery::ShmEventRing* producer,
                        std::uint64_t feed_sequence) {
    const std::uint64_t payload = feed_sequence ^ 0xa5a5a5a5ULL;
    sze_recovery::CanonicalEvent event = make_event(
        feed_sequence, static_cast<std::uint8_t>(0x3eU));
    check(writer->append(&event, &payload) == sze_recovery::kJournalOk,
          "journal append failed");
    check(producer->publish(event, &payload), "ring publish failed");
    check(event.event_id == feed_sequence, "event id did not remain contiguous");
}

void consume_payload(sze_recovery::ReplayHandoffConsumer* consumer,
                     std::uint64_t expected_event_id) {
    const auto deadline=sze_recovery::monotonic_time_ns()+1000000000ULL;
    while (sze_recovery::monotonic_time_ns()<deadline) {
        sze_recovery::CanonicalEvent event;
        std::uint64_t payload = 0U;
        const unsigned char* borrowed=nullptr;
        const sze_recovery::ReplayReadStatus status = consumer->next(
            &event, &payload, sizeof(payload), &borrowed);
        if (status == sze_recovery::kReplayReadWouldBlock) {
            std::this_thread::yield();
            continue;
        }
        check(status == sze_recovery::kReplayReadEvent,
              "handoff consumer did not return an event");
        check(event.event_id == expected_event_id,
              "handoff event id changed order");
        std::memcpy(&payload,borrowed,sizeof(payload));
        check(payload == (expected_event_id ^ 0xa5a5a5a5ULL),
              "handoff payload changed");
        return;
    }
    throw std::runtime_error("handoff consumer remained blocked at event "+std::to_string(expected_event_id));
}

void expect_would_block(sze_recovery::ReplayHandoffConsumer* consumer,
                        const char* message) {
    sze_recovery::CanonicalEvent event;
    std::uint64_t payload = 0U;
    const unsigned char* borrowed=nullptr;
    check(consumer->next(&event, &payload, sizeof(payload), &borrowed) ==
              sze_recovery::kReplayReadWouldBlock,
          message);
}

void attach_prefetch(sze_recovery::ReplayHandoffConsumer& consumer,sse_journal::ShmPrefetch* reader) {
    consumer.set_ring_reader([reader](std::uint64_t id,sze_recovery::CanonicalEvent* e,const unsigned char** bytes){
        const sse_journal::ShmPrefetch::Record* record=nullptr;
        const auto status=reader->read(id,&record);
        if(record){*e=record->event;*bytes=record->payload.data();}
        return status;
    });
}

void run_handoff_test(bool prefetched) {
    const std::string directory = temporary_directory();
    const std::string ring_path = directory + "/events.shm";

    sze_recovery::JournalConfig journal;
    journal.directory = directory;
    journal.prefix = "handoff";
    journal.trading_day = 20260908U;
    journal.source_id = 89U;
    journal.segment_bytes = 65536U;
    journal.max_payload_bytes = sizeof(std::uint64_t);
    journal.generation = 0x12345678U;

    sze_recovery::RingConfig ring;
    ring.path = ring_path;
    ring.trading_day = journal.trading_day;
    ring.source_id = journal.source_id;
    ring.capacity = 4U;
    ring.max_payload_bytes = journal.max_payload_bytes;
    ring.generation = journal.generation;

    sze_recovery::JournalWriter writer;
    sze_recovery::ShmEventRing producer;
    std::unique_ptr<sse_journal::ShmPrefetch> first_reader,second_reader;
    try {
        check(writer.open(journal).status == sze_recovery::kJournalOk,
              "handoff journal open failed");
        check(writer.publish_continuity(sze_recovery::kContinuityValid,
                                        sze_recovery::kInvalidNone, 0U) ==
                  sze_recovery::kJournalOk,
              "handoff journal continuity failed");
        check(producer.create(ring), "handoff ring create failed");
        producer.publish_state(sze_recovery::kContinuityValid,
                               sze_recovery::kReadinessLiveReady,
                               sze_recovery::kInvalidNone, 0U, 0U);

        for (std::uint64_t sequence = 1U; sequence <= 3U; ++sequence)
            append_and_publish(&writer, &producer, sequence);

        sze_recovery::ReplayHandoffConsumer consumer;
        check(consumer.open(journal, ring_path, false),
              "late reader open failed");
        if(prefetched){first_reader.reset(new sse_journal::ShmPrefetch(producer,sched_getcpu()));attach_prefetch(consumer,first_reader.get());}
        check(consumer.mode() == sze_recovery::kReplayJournal,
              "late reader did not begin with journal replay");
        check(producer.readiness_state() == sze_recovery::kReadinessLiveReady,
              "read-only consumer changed producer readiness");
        for (std::uint64_t event_id = 1U; event_id <= 3U; ++event_id)
            consume_payload(&consumer, event_id);
        // Metrics are not a state checkpoint. A new book/model process must
        // reconstruct its state even if another reader published a cursor.
        producer.publish_replay_metrics(3U, 0U, 0U, 0U, 0U, 0U);

        expect_would_block(&consumer, "journal EOF did not wait for handoff");
        check(consumer.mode() == sze_recovery::kReplayHandoff,
              "journal EOF did not enter handoff mode");
        append_and_publish(&writer, &producer, 4U);
        consume_payload(&consumer, 4U);
        check(consumer.mode() == sze_recovery::kReplayLive,
              "handoff did not become live");
        check(producer.readiness_state() == sze_recovery::kReadinessLiveReady,
              "read-only consumer changed live readiness");
        append_and_publish(&writer, &producer, 5U);
        consume_payload(&consumer, 5U);
        if(first_reader)first_reader->stop();
        consumer.close();

        // A restarted predictor always starts from journal event one and then
        // performs the same handoff, so recovery does not depend on old state.
        sze_recovery::ReplayHandoffConsumer restarted;
        check(restarted.open(journal, ring_path, false),
              "restarted reader open failed");
        if(prefetched){second_reader.reset(new sse_journal::ShmPrefetch(producer,sched_getcpu()));attach_prefetch(restarted,second_reader.get());}
        check(restarted.next_event_id() == 1U,
              "fresh reader skipped state history using a metrics cursor");
        for (std::uint64_t event_id = 1U; event_id <= 5U; ++event_id)
            consume_payload(&restarted, event_id);
        expect_would_block(&restarted, "restarted reader skipped handoff");
        check(restarted.mode() == sze_recovery::kReplayHandoff,
              "restarted reader did not enter handoff mode");
        append_and_publish(&writer, &producer, 6U);
        consume_payload(&restarted, 6U);
        check(restarted.mode() == sze_recovery::kReplayLive,
              "restarted reader did not become live");

        // Let the live reader fall behind the four-slot ring. The consumer
        // must seek the journal instead of accepting a discontinuous ring read.
        if(second_reader)second_reader->stop();
        for (std::uint64_t sequence = 7U; sequence <= 11U; ++sequence)
            append_and_publish(&writer, &producer, sequence);
        for(unsigned attempt=0;restarted.mode()!=sze_recovery::kReplayJournal && attempt<10000;++attempt){
            expect_would_block(&restarted,"ring overrun did not fall back to journal");std::this_thread::yield();
        }
        check(restarted.mode() == sze_recovery::kReplayJournal,
              "ring overrun did not return to journal replay");
        check(restarted.ring_overruns() == 1U,
              "ring overrun metric was not recorded");
        consume_payload(&restarted, 7U);

        // A borrowed slot survives capture-ring wrap and a full prefetch queue.
        append_and_publish(&writer,&producer,12U);
        sse_journal::ShmPrefetch held(producer,sched_getcpu(),1);
        const sse_journal::ShmPrefetch::Record* view=nullptr;
        for(unsigned n=0;n<100000 && !view;++n){held.read(12,&view);std::this_thread::yield();}
        check(view && view->event.event_id==12,"prefetch did not expose first record");
        for(std::uint64_t n=13;n<=18;++n)append_and_publish(&writer,&producer,n);
        std::uint64_t held_payload=0;std::memcpy(&held_payload,view->payload.data(),sizeof(held_payload));
        check(held_payload==(12U^0xa5a5a5a5ULL),"borrowed payload was overwritten");
        held.stop(); // Must join even while the one-slot queue is full.
        auto held_status=sze_recovery::kRingReadNotReady;
        for(unsigned n=0;n<100000 && held_status==sze_recovery::kRingReadNotReady;++n){held_status=held.read(13,&view);std::this_thread::yield();}
        check(held_status==sze_recovery::kRingReadOverrun,"prefetch hid a capture-ring overrun");
        held.stop();

        sze_recovery::ShmRingHeader* header = const_cast<sze_recovery::ShmRingHeader*>(
            producer.header());
        check(header != 0, "ring header disappeared");
        header->generation += 1U;
        sze_recovery::CanonicalEvent event;
        std::uint64_t payload = 0U;
        check(restarted.next(&event, &payload, sizeof(payload)) ==
                  sze_recovery::kReplayReadInvalid,
              "generation mismatch was accepted");
        if(second_reader)second_reader->stop();
        restarted.close();

        // A configuration generation mismatch is rejected before any event is
        // exposed, independently of the live ring mutation above.
        sze_recovery::JournalConfig wrong_generation = journal;
        wrong_generation.generation += 1U;
        sze_recovery::ReplayHandoffConsumer rejected;
        check(!rejected.open(wrong_generation, ring_path, false),
              "wrong journal generation was accepted");
        rejected.close();

        check(writer.close(true) == sze_recovery::kJournalOk,
              "handoff journal clean close failed");
        producer.close();
    } catch (...) {
        if(first_reader)first_reader->stop();if(second_reader)second_reader->stop();
        producer.close();
        (void)writer.close(false);
        remove_journal(journal, 8U);
        (void)::unlink(ring_path.c_str());
        (void)::rmdir(directory.c_str());
        throw;
    }
    remove_journal(journal, 8U);
    (void)::unlink(ring_path.c_str());
    check(::rmdir(directory.c_str()) == 0, "handoff test directory cleanup failed");
}

}  // namespace

int main() {
    try {
        run_handoff_test(false);
        run_handoff_test(true);
        std::cout << "sse_journal_handoff_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sse_journal_handoff_test: " << error.what() << '\n';
        return 1;
    }
}
