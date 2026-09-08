#include "common/recovery/SZERecoverable.h"
#include "sse/runtime/sse_cpu_affinity.h"
#include "sse/runtime/sse_journal_transport.h"
#include "sse/runtime/sse_stream_processor.h"
#include "tests/sse/sse_test_artifacts.h"

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

typedef std::vector<unsigned char> Bytes;
typedef deepwin_market_data::StreamEvent Event;
typedef sse_stream::Output Output;
typedef sse_stream::SseStreamProcessor Processor;

void put(Bytes* bytes, std::size_t offset, std::uint64_t value,
         std::size_t width) {
    for (std::size_t i = 0U; i < width; ++i)
        (*bytes)[offset + i] = static_cast<unsigned char>((value >> (i * 8U)) & 255U);
}

void append(Bytes* bytes, const Bytes& value) {
    bytes->insert(bytes->end(), value.begin(), value.end());
}

std::uint32_t wire_time(unsigned seconds) {
    return 9300000U + seconds / 60U * 10000U + seconds % 60U * 100U;
}

Bytes tick(unsigned channel, std::uint64_t sequence, const std::string& code,
           char type = 'T', unsigned seconds = 1U, unsigned side = 0U) {
    Bytes bytes(72U, 0U);
    put(&bytes, 0U, sequence, 4U);
    bytes[8] = 0x3e;
    put(&bytes, 9U, sequence, 8U);
    put(&bytes, 17U, channel, 2U);
    std::memcpy(bytes.data() + 21U, code.data(), 6U);
    put(&bytes, 30U, wire_time(seconds), 4U);
    bytes[34] = static_cast<unsigned char>(type);
    put(&bytes, 35U, type == 'A' && side == 1U ? 0U : 1001U, 8U);
    put(&bytes, 43U, type == 'A' && side == 0U ? 0U : 2001U, 8U);
    put(&bytes, 51U, 10000U, 4U);
    put(&bytes, 55U, type == 'T' ? 100000U : 100000000U, 8U);
    bytes[71] = static_cast<unsigned char>(side);
    return bytes;
}

Bytes opening_packet() {
    Bytes bytes = tick(1U, 1U, "600000", 'A', 0U, 0U);
    append(&bytes, tick(1U, 2U, "600000", 'A', 0U, 1U));
    for (unsigned sequence = 3U; sequence <= 20U; ++sequence)
        append(&bytes, tick(1U, sequence, "600000", 'T', 1U));
    return bytes;
}

sse_tick::DailyStaticMetadataMap metadata() {
    sse_tick::DailyStaticMetadata row;
    row.date = 20260909U;
    row.avg_amount = 100000000.0;
    row.turnover_threshold = 0.1;
    row.free_share = 1000000.0;
    row.pre_close = 10.0;
    row.limit_price = 11.0;
    row.stop_price = 9.0;
    row.has_date = row.has_avg_amount = row.has_turnover_threshold = true;
    row.has_free_share = row.has_pre_close = row.has_limit_price = row.has_stop_price = true;
    row.quality = "journal-handoff-test";
    sse_tick::DailyStaticMetadataMap result;
    result["600000"] = row;
    return result;
}

sse_stream::Auction59Provider auction_provider() {
    return [](const std::string&, std::uint64_t, std::vector<float>* values,
              std::string*) {
        values->assign(59U, 0.0f);
        return true;
    };
}

sse_stream::PipelineConfig parallel_pipeline() {
    sse_stream::PipelineConfig config;
    config.contract = sse_stream::kHardwareBatchV3;
    std::vector<int> cpus;
    const char* requested = std::getenv("SSE_E2E_CPUS");
    const std::string cpu_list = requested ? requested : "112,120,128,136";
    std::string error;
    assert(sse_cpu::parse_cpu_list(cpu_list, &cpus, &error) && cpus.size() == 4U);
    config.book_cpus.assign(cpus.begin(), cpus.begin() + 2);
    config.inference_cpus.assign(cpus.begin() + 2, cpus.end());
    return config;
}

struct ModelFiles {
    std::string directory;
    sse_hybrid_model::Model model;

    ModelFiles() : directory(), model() {
        char path[] = "/tmp/sse-journal-handoff-model-XXXXXX";
        char* made = ::mkdtemp(path);
        assert(made != 0);
        directory = made;
        sse_test_artifacts::write_tick_artifact(directory + "/tick.bin");
        sse_test_artifacts::write_snapshot_artifact(directory + "/baseline.bin", 36U);
        sse_test_artifacts::write_snapshot_artifact(directory + "/auction.bin", 95U);
        sse_test_artifacts::write_scaler(directory + "/baseline.json", 36U);
        sse_test_artifacts::write_scaler(directory + "/auction.json", 95U);
        std::string error;
        assert(model.load(directory + "/tick.bin", directory + "/baseline.bin",
                          directory + "/baseline.json", directory + "/auction.bin",
                          directory + "/auction.json", &error));
    }

    ~ModelFiles() {
        const char* names[] = {"tick.bin", "baseline.bin", "baseline.json",
                               "auction.bin", "auction.json"};
        for (const char* name : names)
            ::unlink((directory + "/" + name).c_str());
        ::rmdir(directory.c_str());
    }
};

struct TransportFiles {
    std::string journal_directory;
    std::string ring_path;
    sse_journal::Config config;
    sze_recovery::JournalWriter writer;
    sze_recovery::ShmEventRing producer_ring;
    sze_recovery::ReplayHandoffConsumer consumer;

    TransportFiles() : journal_directory(), ring_path(), config(), writer(),
                       producer_ring(), consumer() {
        char directory[] = "/tmp/sse-journal-handoff-XXXXXX";
        char* made = ::mkdtemp(directory);
        assert(made != 0);
        journal_directory = made;
        ring_path = std::string("/dev/shm/sse-journal-handoff-") +
                    std::to_string(static_cast<unsigned long long>(::getpid()));

        config.schema_version = 2U;
        config.extended_timestamps = true;
        config.stream.hardware_timestamp_interface = "synthetic-phc-for-test";
        config.stream.idle_gap_ns = 5000U;
        deepwin_market_data::ChannelSpec channel;
        channel.name = "sse_tick";
        channel.group = "239.35.80.9";
        channel.interface_ip = "11.11.11.11";
        channel.port = 37109;
        config.channels.push_back(channel);

        config.journal.directory = journal_directory;
        config.journal.prefix = "sse";
        config.journal.trading_day = 20260909U;
        config.journal.source_id = 89U;
        config.journal.segment_bytes = 1U << 20U;
        config.journal.max_payload_bytes = static_cast<std::uint32_t>(
            sse_journal::stored_header_bytes(config) + sse_journal::kMaxDatagram);
        config.journal.generation = 0x202609090001ULL;
        config.journal.min_free_bytes_after_allocate = 0U;

        config.ring.path = ring_path;
        config.ring.trading_day = config.journal.trading_day;
        config.ring.source_id = config.journal.source_id;
        config.ring.capacity = 64U;
        config.ring.max_payload_bytes = config.journal.max_payload_bytes;
        config.ring.generation = config.journal.generation;

        const sze_recovery::JournalOpenResult opened = writer.open(config.journal);
        assert(opened.status == sze_recovery::kJournalOk && !opened.existing);
        assert(writer.publish_continuity(sze_recovery::kContinuityValid,
                                         sze_recovery::kInvalidNone, 0U) ==
               sze_recovery::kJournalOk);
        assert(producer_ring.create(config.ring));
        producer_ring.publish_state(sze_recovery::kContinuityValid,
                                    sze_recovery::kReadinessLiveReady,
                                    sze_recovery::kInvalidNone, 0U, 0U);
    }

    ~TransportFiles() {
        consumer.close();
        producer_ring.publish_continuity(sze_recovery::kContinuityInvalid,
                                          sze_recovery::kInvalidReceiverStopped,
                                          0U, 0U);
        producer_ring.close();
        writer.close(false);
        ::unlink(ring_path.c_str());
        const std::string segment = writer.segment_path(0U);
        ::unlink(segment.c_str());
        ::rmdir(journal_directory.c_str());
    }
};

Event stream_event(const Bytes& bytes, std::uint64_t sequence,
                   std::uint64_t monotonic, std::uint64_t hardware) {
    Event event = {};
    event.kind = deepwin_market_data::kDatagramEvent;
    event.sequence = sequence;
    event.monotonic_ns = monotonic;
    event.realtime_ns = 100000000000ULL + monotonic;
    event.receive_batch = sequence;
    event.batch_index = 0U;
    event.batch_size = 1U;
    event.channel_id = 0U;
    event.timestamp_flags = static_cast<std::uint16_t>(
        deepwin_market_data::kKernelRealtimeTimestamp |
        deepwin_market_data::kHardwareReceiveTimestamp |
        deepwin_market_data::kHardwareTimestampRequested);
    event.hardware_ns = hardware;
    event.application_realtime_ns = event.realtime_ns;
    event.hardware_clock_index = 3;
    event.data = bytes.data();
    event.size = bytes.size();
    return event;
}

sse_journal::StoredEvent encode_event(const sse_journal::Config& config,
                                      const Bytes& bytes, std::uint64_t sequence,
                                      std::uint64_t monotonic,
                                      std::uint64_t hardware) {
    sse_journal::StoredEvent stored = {};
    const Event event = stream_event(bytes, sequence, monotonic, hardware);
    sse_journal::encode(event, config, &stored);
    assert(stored.event.event_id == sequence);
    assert(stored.event.source_id == 89U);
    assert(stored.event.payload_size ==
           sse_journal::stored_header_bytes(config) + bytes.size());
    return stored;
}

void append_journal(sze_recovery::JournalWriter* writer,
                    const sse_journal::StoredEvent& stored) {
    sze_recovery::CanonicalEvent event = stored.event;
    assert(writer->append(&event, stored.payload) == sze_recovery::kJournalOk);
    assert(event.event_id == stored.event.event_id);
}

void publish_ring(sze_recovery::ShmEventRing* ring,
                  const sse_journal::StoredEvent& stored) {
    assert(ring->publish(stored.event, stored.payload));
}

void assert_rejects_missing_hardware(const sse_tick::DailyStaticMetadataMap& rows,
                                    const Event& source) {
    Event missing = source;
    missing.hardware_ns = 0U;
    missing.timestamp_flags = static_cast<std::uint16_t>(
        missing.timestamp_flags & ~deepwin_market_data::kHardwareReceiveTimestamp);
    sse_stream::PipelineConfig config;
    config.contract = sse_stream::kHardwareBatchV3;
    Processor processor(rows, 0, true, [](const Output&) {}, auction_provider(), config);
    bool rejected = false;
    try {
        processor.on_event(missing);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
    assert(processor.invalid());
}

}  // namespace

int main() {
    ModelFiles models;
    TransportFiles transport;
    const sse_tick::DailyStaticMetadataMap rows = metadata();
    const Bytes first_wire = opening_packet();
    const Bytes second_wire = tick(1U, 21U, "600000", 'T', 2U);
    const sse_journal::StoredEvent first = encode_event(
        transport.config, first_wire, 1U, 100000000U, 1000000U);
    const sse_journal::StoredEvent second = encode_event(
        transport.config, second_wire, 2U, 100010000U, 1005000U);

    // The first canonical v2 record is durable in the journal and visible in
    // the ring. All clock values here are synthetic test metadata.
    append_journal(&transport.writer, first);
    publish_ring(&transport.producer_ring, first);
    assert(transport.consumer.open(transport.config.journal,
                                   transport.config.ring.path, false));
    assert(transport.consumer.mode() == sze_recovery::kReplayJournal);

    std::vector<Output> outputs;
    Processor processor(rows, &models.model, false,
        [&](const Output& output) { outputs.push_back(output); },
        auction_provider(), parallel_pipeline());
    std::vector<unsigned char> payload(transport.config.journal.max_payload_bytes, 0U);
    sze_recovery::CanonicalEvent canonical = {};
    assert(transport.consumer.next(&canonical, payload.data(), payload.size()) ==
           sze_recovery::kReplayReadEvent);
    const Event first_event = sse_journal::decode(
        canonical, payload.data(), transport.config);
    assert(transport.consumer.replayed_events() == 1U);
    assert(transport.consumer.live_events() == 0U);
    processor.on_event(first_event);

    // Once the journal reader is caught up, the consumer enters handoff but
    // waits for the next ring publication rather than inventing an event.
    assert(transport.consumer.next(&canonical, payload.data(), payload.size()) ==
           sze_recovery::kReplayReadWouldBlock);
    assert(transport.consumer.mode() == sze_recovery::kReplayHandoff);
    processor.poll_outputs();
    publish_ring(&transport.producer_ring, second);
    assert(transport.consumer.next(&canonical, payload.data(), payload.size()) ==
           sze_recovery::kReplayReadEvent);
    assert(transport.consumer.mode() == sze_recovery::kReplayLive);
    assert(transport.consumer.replayed_events() == 1U);
    assert(transport.consumer.live_events() == 1U);
    const Event second_event = sse_journal::decode(
        canonical, payload.data(), transport.config);
    processor.on_event(second_event);

    // The second packet's PHC gap is exactly 5,000 ns. The first batch is
    // closed asynchronously; only the owner thread may dispatch its outputs.
    for (unsigned attempt = 0U; attempt < 200000U && outputs.size() < 2U; ++attempt) {
        processor.poll_outputs();
        if (outputs.size() < 2U) std::this_thread::yield();
    }
    assert(outputs.size() == 2U);
    assert(outputs[0].kind == sse_stream::kTickOutput);
    assert(outputs[0].tick.prediction_valid);
    assert(outputs[0].tick.event.tick_index == 20U);
    assert(outputs[0].tick.provenance.processing_contract == sse_stream::kHardwareBatchV3);
    assert(outputs[0].tick.provenance.stream_channel_id == 0U);
    assert(outputs[1].kind == sse_stream::kBatchEndOutput);
    assert(outputs[1].batch_end.stream_channel_id == 0U);
    assert(outputs[1].batch_end.last_hardware_ns == 1000000U);
    assert(outputs[1].batch_end.reason == sse_live_sampling::kBatchClosedByNextEvent);
    assert(outputs[1].batch_end.prediction_count == 1U);
    assert(outputs[0].tick.provenance.batch_id == outputs[1].batch_end.batch_id);
    assert(processor.pipeline_stats().inferred_samples == 1U);

    // Event 2 opened a new batch. finish drains the confirmed first batch but
    // never manufactures a close/prediction for that final open batch.
    processor.finish();
    processor.finish();
    assert(outputs.size() == 2U);
    assert(processor.pipeline_stats().inferred_samples == 1U);

    // Complete the journal after the SHM handoff, as the producer would after
    // its asynchronous journal worker catches up. The consumer already chose
    // live mode and therefore does not replay this duplicate record.
    append_journal(&transport.writer, second);
    assert(transport.writer.flush(true) == sze_recovery::kJournalOk);
    assert(transport.writer.close(true) == sze_recovery::kJournalOk);
    assert_rejects_missing_hardware(rows, first_event);
    std::cout << "sse_pipeline_journal_handoff_test: PASS"
              << " replayed=" << transport.consumer.replayed_events()
              << " live=" << transport.consumer.live_events()
              << " confirmed_outputs=" << outputs.size()
              << " unsealed_batch_predictions=0\n";
    return 0;
}
