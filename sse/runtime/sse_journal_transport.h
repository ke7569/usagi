#ifndef USAGI_SSE_JOURNAL_TRANSPORT_H
#define USAGI_SSE_JOURNAL_TRANSPORT_H

#include "common/recovery/SZERecoverable.h"
#include "common/config/StreamInputConfig.h"
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace sse_journal {

// The existing SZEJNL journal/ring container is unchanged. This source-89
// payload identifies the Shanghai stream metadata needed by main's processor.
// It is distinct from the historical experiment's fixed 504-byte Event.
static const std::uint32_t kPayloadMagic = 0x31534853U;
static const std::uint16_t kPayloadVersion = 1;
static const std::uint16_t kPayloadVersionV2 = 2;
static const std::size_t kMaxDatagram = 8192;
// The capture idle marker is separate from the 5us PHC batch boundary used
// by the stream processor. Keep the historical 100us value readable so old
// recordings can still be replayed while new captures use 5us by default.
static const std::uint64_t kDefaultIdleGapNanoseconds = 5000ULL;
static const std::uint64_t kHistoricalIdleGapNanoseconds = 100000ULL;

inline bool is_supported_idle_gap_ns(std::uint64_t value) {
    return value == kDefaultIdleGapNanoseconds ||
           value == kHistoricalIdleGapNanoseconds;
}

struct PayloadHeader {
    std::uint32_t magic;
    std::uint16_t version, kind;
    std::uint64_t realtime_ns, receive_batch;
    std::uint32_t channel_id, batch_index, batch_size, source_ipv4;
    std::uint16_t source_port, timestamp_flags;
    std::uint32_t payload_bytes;
};
static_assert(sizeof(PayloadHeader) == 48, "Shanghai journal payload header ABI");
struct PayloadHeaderV2 {
    PayloadHeader prefix;
    std::uint64_t hardware_ns;
    std::uint64_t application_realtime_ns;
    std::int32_t hardware_clock_index;
    std::uint32_t reserved;
    std::uint64_t reserved2;
};
static_assert(sizeof(PayloadHeaderV2) == 80, "Shanghai journal v2 payload header ABI");
static const std::uint16_t kKnownTimestampFlags =
    static_cast<std::uint16_t>(deepwin_market_data::kKernelRealtimeTimestamp |
                               deepwin_market_data::kUserspaceRealtimeTimestamp |
                               deepwin_market_data::kHardwareReceiveTimestamp |
                               deepwin_market_data::kHardwareTimestampRequested);
struct StoredEvent {
    sze_recovery::CanonicalEvent event;
    unsigned char payload[sizeof(PayloadHeaderV2) + kMaxDatagram];
};

inline std::string boot_id() {
    std::ifstream source("/proc/sys/kernel/random/boot_id");
    std::string value;
    std::getline(source, value);
    if (value.empty()) throw std::runtime_error("cannot read host boot identity");
    return value;
}

struct Config {
    sze_recovery::JournalConfig journal;
    sze_recovery::RingConfig ring;
    deepwin_market_data::StreamOptions stream;
    std::vector<deepwin_market_data::ChannelSpec> channels;
    std::size_t journal_queue_capacity;
    int journal_cpu, prediction_cpu;
    unsigned flush_interval_ms;
    long duration_ms;
    std::string boot;
    bool extended_timestamps;
    Config() : journal_queue_capacity(32768), journal_cpu(-1), prediction_cpu(-1),
               flush_interval_ms(100), duration_ms(0), extended_timestamps(false) {}
};

inline std::size_t stored_header_bytes(const Config& config) {
    return config.extended_timestamps ? sizeof(PayloadHeaderV2) : sizeof(PayloadHeader);
}

inline Config load(const std::string& path, bool allow_historical_boot = false) {
    const nlohmann::json value = load_stream_json(path);
    stream_input::fields(value, {"schema_version", "payload_format", "trading_day", "source_id",
        "generation", "boot_id", "journal_directory", "journal_prefix", "shm_path",
        "segment_bytes", "min_free_bytes_after_allocate", "ring_capacity", "journal_queue_capacity",
        "queue_capacity", "max_datagram_bytes", "receive_batch_size", "receive_busy_poll", "dispatch_busy_poll", "receive_buffer_bytes",
        "idle_gap_ns", "receive_cpu", "dispatch_cpu", "journal_cpu", "prediction_cpu",
        "flush_interval_ms", "duration_ms", "hardware_timestamp_interface", "channels"});
    if (value.at("schema_version") != 1)
        throw std::runtime_error("unsupported SSE journal transport configuration");
    Config config;
    const std::string payload_format = value.at("payload_format").get<std::string>();
    if (payload_format == "sse-stream-v1") {
        config.extended_timestamps = false;
    } else if (payload_format == "sse-stream-v2") {
        config.extended_timestamps = true;
    } else {
        throw std::runtime_error("unsupported SSE journal payload format");
    }
    config.journal.trading_day = static_cast<std::uint32_t>(stream_input::uint_value(value.at("trading_day")));
    if (config.journal.trading_day < 20000101U || config.journal.trading_day > 99991231U)
        throw std::runtime_error("invalid journal trading day");
    if (stream_input::uint_value(value.at("source_id")) != 89U)
        throw std::runtime_error("Shanghai journal source_id must be 89");
    config.journal.source_id = 89;
    config.journal.generation = stream_input::uint_value(value.at("generation"));
    if (!config.journal.generation) throw std::runtime_error("explicit journal generation required");
    config.boot = value.at("boot_id").get<std::string>();
    if (!allow_historical_boot && config.boot != boot_id()) throw std::runtime_error("journal belongs to another host boot");
    config.journal.directory = value.at("journal_directory").get<std::string>();
    config.journal.prefix = value.value("journal_prefix", std::string("sse"));
    if (config.journal.directory.empty() || config.journal.directory[0] != '/' ||
        config.journal.prefix.empty() || config.journal.prefix.find('/') != std::string::npos)
        throw std::runtime_error("absolute journal directory and simple prefix required");
    config.stream.hardware_timestamp_interface = value.value(
        "hardware_timestamp_interface", std::string());
    if (!config.extended_timestamps && !config.stream.hardware_timestamp_interface.empty())
        throw std::runtime_error("hardware timestamp interface requires sse-stream-v2");
    config.journal.max_payload_bytes = stored_header_bytes(config) + kMaxDatagram;
    config.journal.segment_bytes = 256ULL << 20;
    stream_input::optional_uint(value, "segment_bytes", &config.journal.segment_bytes);
    stream_input::optional_uint(value, "min_free_bytes_after_allocate", &config.journal.min_free_bytes_after_allocate);
    config.ring.path = value.at("shm_path").get<std::string>();
    if (config.ring.path.empty() || config.ring.path[0] != '/')
        throw std::runtime_error("absolute SHM path required");
    config.ring.trading_day = config.journal.trading_day;
    config.ring.source_id = config.journal.source_id;
    config.ring.generation = config.journal.generation;
    config.ring.max_payload_bytes = config.journal.max_payload_bytes;
    config.ring.capacity = 65536;
    stream_input::optional_uint(value, "ring_capacity", &config.ring.capacity);
    stream_input::optional_uint(value, "journal_queue_capacity", &config.journal_queue_capacity);
    stream_input::optional_uint(value, "flush_interval_ms", &config.flush_interval_ms);
    stream_input::optional_uint(value, "duration_ms", &config.duration_ms);
    stream_input::cpu(value, "journal_cpu", &config.journal_cpu);
    stream_input::cpu(value, "prediction_cpu", &config.prediction_cpu);
    if(value.count("receive_busy_poll")) {
        if(!value.at("receive_busy_poll").is_boolean())throw std::runtime_error("receive_busy_poll must be boolean");
        config.stream.receive_busy_poll=value.at("receive_busy_poll").get<bool>();
    }
    if(value.count("dispatch_busy_poll")) {
        if(!value.at("dispatch_busy_poll").is_boolean())throw std::runtime_error("dispatch_busy_poll must be boolean");
        config.stream.dispatch_busy_poll=value.at("dispatch_busy_poll").get<bool>();
    }
    config.stream.recording_directory.clear();
    config.stream.recording_required = false;
    config.stream.idle_gap_ns = kDefaultIdleGapNanoseconds;
    config.stream.queue_capacity = 32768;
    config.stream.max_datagram_bytes = kMaxDatagram;
    config.stream.receive_buffer_bytes = 67108864;
    stream_input::optional_uint(value, "queue_capacity", &config.stream.queue_capacity);
    stream_input::optional_uint(value, "max_datagram_bytes", &config.stream.max_datagram_bytes);
    stream_input::optional_uint(value, "receive_batch_size", &config.stream.receive_batch_size);
    stream_input::optional_uint(value, "receive_buffer_bytes", &config.stream.receive_buffer_bytes);
    stream_input::optional_uint(value, "idle_gap_ns", &config.stream.idle_gap_ns);
    stream_input::cpu(value, "receive_cpu", &config.stream.receive_cpu);
    stream_input::cpu(value, "dispatch_cpu", &config.stream.dispatch_cpu);
    if (!is_supported_idle_gap_ns(config.stream.idle_gap_ns) ||
        config.stream.max_datagram_bytes > kMaxDatagram ||
        !config.stream.max_datagram_bytes || !config.ring.capacity || !config.journal_queue_capacity ||
        !config.flush_interval_ms)
        throw std::runtime_error("invalid SSE journal capacities/timing");
    const nlohmann::json& channels = value.at("channels");
    if (!channels.is_array() || channels.empty() || channels.size() > 64)
        throw std::runtime_error("journal requires 1..64 subscriptions");
    for (const auto& input : channels) {
        stream_input::fields(input, {"name", "group", "port", "interface_ip"});
        deepwin_market_data::ChannelSpec channel;
        channel.name = input.at("name").get<std::string>();
        channel.group = input.at("group").get<std::string>();
        channel.interface_ip = input.at("interface_ip").get<std::string>();
        const std::uint64_t port = stream_input::uint_value(input.at("port"));
        if (!port || port > 65535) throw std::runtime_error("invalid journal UDP port");
        channel.port = static_cast<int>(port);
        config.channels.push_back(channel);
    }
    return config;
}

inline void encode(const deepwin_market_data::StreamEvent& input, const Config& config, StoredEvent* output) {
    if (!output || input.size > kMaxDatagram || (input.size && !input.data) ||
        (input.kind != deepwin_market_data::kDatagramEvent && input.kind != deepwin_market_data::kIdleEvent) ||
        (input.kind == deepwin_market_data::kIdleEvent && input.size))
        throw std::runtime_error("invalid Shanghai stream event for journal");
    if (input.timestamp_flags & static_cast<std::uint16_t>(~kKnownTimestampFlags) ||
        (input.timestamp_flags & deepwin_market_data::kHardwareReceiveTimestamp &&
         (!(input.timestamp_flags & deepwin_market_data::kHardwareTimestampRequested) ||
          input.hardware_ns == 0U)))
        throw std::runtime_error("invalid Shanghai stream timestamp flags");
    if (input.timestamp_flags & deepwin_market_data::kHardwareTimestampRequested &&
        (input.hardware_clock_index < 0 || input.application_realtime_ns == 0U ||
         ((input.timestamp_flags & deepwin_market_data::kHardwareReceiveTimestamp) != 0U) !=
          (input.hardware_ns != 0U)))
        throw std::runtime_error("invalid requested hardware timestamp fields");
    if (!config.extended_timestamps &&
        (!config.stream.hardware_timestamp_interface.empty() ||
         (input.timestamp_flags & (deepwin_market_data::kHardwareReceiveTimestamp |
                                   deepwin_market_data::kHardwareTimestampRequested))))
        throw std::runtime_error("hardware timestamp fields require sse-stream-v2");
    output->event = sze_recovery::CanonicalEvent();
    output->event.event_id = input.sequence;
    output->event.feed_sequence = input.sequence;
    output->event.channel_sequence = input.receive_batch;
    output->event.receive_mono_ns = input.monotonic_ns;
    output->event.trading_day = config.journal.trading_day;
    output->event.source_id = 89;
    output->event.channel_number = static_cast<std::uint16_t>(input.channel_id);
    output->event.record_kind = sze_recovery::kRecordMarketData;
    output->event.message_type = static_cast<std::uint8_t>(input.kind);
    const std::size_t header_bytes = stored_header_bytes(config);
    output->event.payload_size = static_cast<std::uint16_t>(header_bytes + input.size);
    PayloadHeader header = {};
    header.magic = kPayloadMagic;
    header.version = config.extended_timestamps ? kPayloadVersionV2 : kPayloadVersion;
    header.kind = static_cast<std::uint16_t>(input.kind);
    header.realtime_ns = input.realtime_ns; header.receive_batch = input.receive_batch;
    header.channel_id = input.channel_id; header.batch_index = input.batch_index;
    header.batch_size = input.batch_size; header.source_ipv4 = input.source_ipv4;
    header.source_port = input.source_port; header.timestamp_flags = input.timestamp_flags;
    header.payload_bytes = static_cast<std::uint32_t>(input.size);
    if (config.extended_timestamps) {
        PayloadHeaderV2 extended = {};
        extended.prefix = header;
        extended.hardware_clock_index = -1;
        if (input.timestamp_flags & deepwin_market_data::kHardwareTimestampRequested) {
            extended.hardware_ns = input.hardware_ns;
            extended.application_realtime_ns = input.application_realtime_ns;
            extended.hardware_clock_index = input.hardware_clock_index;
        } else {
            extended.application_realtime_ns = input.kind == deepwin_market_data::kIdleEvent
                ? input.realtime_ns : 0U;
        }
        std::memcpy(output->payload, &extended, sizeof(extended));
    } else {
        std::memcpy(output->payload, &header, sizeof(header));
    }
    if (input.size) std::memcpy(output->payload + header_bytes, input.data, input.size);
    output->event.payload_crc32 = sze_recovery::crc32(output->payload, output->event.payload_size);
}

inline deepwin_market_data::StreamEvent decode(const sze_recovery::CanonicalEvent& event,
                                               const void* payload, const Config& config) {
    const std::size_t header_bytes = stored_header_bytes(config);
    if (!payload || event.payload_size < header_bytes ||
        event.payload_size > config.journal.max_payload_bytes || event.source_id != 89 ||
        event.trading_day != config.journal.trading_day || event.record_kind != sze_recovery::kRecordMarketData)
        throw std::runtime_error("incompatible Shanghai journal event");
    PayloadHeader header;
    std::memcpy(&header, payload, sizeof(header));
    const std::uint16_t expected_version = config.extended_timestamps
        ? kPayloadVersionV2 : kPayloadVersion;
    if (header.magic != kPayloadMagic || header.version != expected_version ||
        header.payload_bytes > kMaxDatagram ||
        static_cast<std::size_t>(header.payload_bytes) + header_bytes != event.payload_size ||
        header.kind != event.message_type || header.channel_id >= config.channels.size() ||
        header.timestamp_flags & static_cast<std::uint16_t>(~kKnownTimestampFlags) ||
        (header.timestamp_flags & deepwin_market_data::kHardwareReceiveTimestamp &&
         !(header.timestamp_flags & deepwin_market_data::kHardwareTimestampRequested)) ||
        (!config.extended_timestamps &&
         (header.timestamp_flags & (deepwin_market_data::kHardwareReceiveTimestamp |
                                    deepwin_market_data::kHardwareTimestampRequested))) ||
        (header.kind != deepwin_market_data::kDatagramEvent && header.kind != deepwin_market_data::kIdleEvent) ||
        (header.kind == deepwin_market_data::kIdleEvent && header.payload_bytes))
        throw std::runtime_error("invalid Shanghai journal payload metadata");
    PayloadHeaderV2 extended = {};
    if (config.extended_timestamps) {
        std::memcpy(&extended, payload, sizeof(extended));
        if (extended.prefix.magic != header.magic ||
            extended.prefix.version != header.version ||
            extended.prefix.kind != header.kind ||
            extended.prefix.payload_bytes != header.payload_bytes ||
            extended.reserved != 0U || extended.reserved2 != 0U)
            throw std::runtime_error("invalid Shanghai journal v2 timestamp extension");
        if (!(header.timestamp_flags & deepwin_market_data::kHardwareTimestampRequested) &&
            (extended.hardware_ns != 0U || extended.hardware_clock_index != -1 ||
             extended.application_realtime_ns !=
                 (header.kind == deepwin_market_data::kIdleEvent
                      ? header.realtime_ns : 0U)))
            throw std::runtime_error("unrequested hardware timestamp fields are populated");
        if (header.timestamp_flags & deepwin_market_data::kHardwareTimestampRequested) {
            if (extended.hardware_clock_index < 0 || extended.application_realtime_ns == 0U ||
                (((header.timestamp_flags & deepwin_market_data::kHardwareReceiveTimestamp) != 0U) !=
                 (extended.hardware_ns != 0U)))
                throw std::runtime_error("requested hardware timestamp fields are inconsistent");
        }
    }
    deepwin_market_data::StreamEvent output = {};
    output.kind = static_cast<deepwin_market_data::StreamEventKind>(header.kind);
    output.sequence = event.event_id; output.monotonic_ns = event.receive_mono_ns;
    output.realtime_ns = header.realtime_ns; output.receive_batch = header.receive_batch;
    output.channel_id = header.channel_id; output.batch_index = header.batch_index;
    output.batch_size = header.batch_size; output.source_ipv4 = header.source_ipv4;
    output.source_port = header.source_port; output.timestamp_flags = header.timestamp_flags;
    output.size = header.payload_bytes;
    output.hardware_clock_index = -1;
    if (config.extended_timestamps) {
        output.hardware_ns = extended.hardware_ns;
        output.application_realtime_ns = extended.application_realtime_ns;
        output.hardware_clock_index = extended.hardware_clock_index;
    }
    output.data = output.size ? static_cast<const unsigned char*>(payload) + header_bytes : 0;
    return output;
}
}  // namespace sse_journal
#endif
