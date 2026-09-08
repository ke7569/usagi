#include "sse/runtime/sse_journal_transport.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {

std::string write_config(const std::string& path, unsigned schema,
                         const std::string& payload_format,
                         const std::string& hardware_interface,
                         std::uint64_t idle_gap_ns) {
    nlohmann::json value = {
        {"schema_version", schema},
        {"payload_format", payload_format},
        {"trading_day", 20260908},
        {"source_id", 89},
        {"generation", 1},
        {"boot_id", sse_journal::boot_id()},
        {"journal_directory", "/tmp/sse-wait-contract-journal"},
        {"journal_prefix", "sse"},
        {"shm_path", "/dev/shm/sse-wait-contract-ring"},
        {"segment_bytes", 268435456},
        {"min_free_bytes_after_allocate", 0},
        {"ring_capacity", 65536},
        {"journal_queue_capacity", 32768},
        {"queue_capacity", 32768},
        {"max_datagram_bytes", 8192},
        {"receive_batch_size", 64},
        {"receive_buffer_bytes", 67108864},
        {"idle_gap_ns", idle_gap_ns},
        {"receive_cpu", -1},
        {"dispatch_cpu", -1},
        {"journal_cpu", -1},
        {"prediction_cpu", -1},
        {"flush_interval_ms", 100},
        {"duration_ms", 0},
        {"hardware_timestamp_interface", hardware_interface},
        {"channels", nlohmann::json::array({
            nlohmann::json{{"name", "sse_tick"}, {"group", "239.35.80.9"},
                           {"port", 37109}, {"interface_ip", "11.11.11.11"}}})}
    };
    std::ofstream output(path.c_str(), std::ios::trunc);
    output << value.dump(2);
    output.close();
    return path;
}

bool rejects(const std::string& path) {
    try {
        (void)sse_journal::load(path);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const std::string prefix = std::string("/tmp/sse_wait_transport_contract_") +
                               std::to_string(static_cast<long long>(::getpid()));
    const std::string v1 = write_config(prefix + ".v1.json", 1U, "sse-stream-v2",
                                         "hqh-p1-k2", 100000U);
    const std::string v1_bad_idle = write_config(prefix + ".v1-bad.json", 1U,
                                                  "sse-stream-v2", "hqh-p1-k2", 5000U);
    const std::string v2 = write_config(prefix + ".v2.json", 2U, "sse-stream-v2",
                                         "hqh-p1-k2", 5000U);
    const std::string v2_no_hardware = write_config(prefix + ".v2-no-hw.json", 2U,
                                                     "sse-stream-v2", "", 5000U);
    const std::string v2_old_payload = write_config(prefix + ".v2-old.json", 2U,
                                                    "sse-stream-v1", "", 5000U);
    const std::string v1_plain = write_config(prefix + ".v1-plain.json", 1U,
                                              "sse-stream-v1", "", 100000U);

    const sse_journal::Config old_config = sse_journal::load(v1);
    assert(old_config.schema_version == 1U);
    assert(old_config.stream.idle_gap_ns == 100000U);
    assert(old_config.extended_timestamps);
    const sse_journal::Config short_config = sse_journal::load(v2);
    assert(short_config.schema_version == 2U);
    assert(short_config.stream.idle_gap_ns == 5000U);
    assert(short_config.extended_timestamps);
    const sse_journal::Config plain_config = sse_journal::load(v1_plain);
    assert(plain_config.schema_version == 1U);
    assert(!plain_config.extended_timestamps);
    assert(rejects(v1_bad_idle));
    assert(rejects(v2_no_hardware));
    assert(rejects(v2_old_payload));

    deepwin_market_data::StreamEvent idle = {};
    idle.kind = deepwin_market_data::kIdleEvent;
    idle.sequence = 1U;
    idle.monotonic_ns = 1000000U;
    idle.realtime_ns = 2000000U;
    sse_journal::StoredEvent stored_idle = {};
    sse_journal::encode(idle, short_config, &stored_idle);
    const deepwin_market_data::StreamEvent decoded_idle =
        sse_journal::decode(stored_idle.event, stored_idle.payload, short_config);
    assert(decoded_idle.kind == deepwin_market_data::kIdleEvent);
    assert(decoded_idle.size == 0U && decoded_idle.data == 0);
    assert(decoded_idle.application_realtime_ns == idle.realtime_ns);

    unsigned char bytes[3] = {7U, 8U, 9U};
    deepwin_market_data::StreamEvent datagram = {};
    datagram.kind = deepwin_market_data::kDatagramEvent;
    datagram.sequence = 2U;
    datagram.monotonic_ns = 1000100U;
    datagram.realtime_ns = 3000000U;
    datagram.receive_batch = 4U;
    datagram.channel_id = 0U;
    datagram.batch_index = 0U;
    datagram.batch_size = 1U;
    datagram.timestamp_flags = static_cast<std::uint16_t>(
        deepwin_market_data::kKernelRealtimeTimestamp |
        deepwin_market_data::kHardwareReceiveTimestamp |
        deepwin_market_data::kHardwareTimestampRequested);
    datagram.hardware_ns = 9000000U;
    datagram.application_realtime_ns = 3000000U;
    datagram.hardware_clock_index = 0;
    datagram.data = bytes;
    datagram.size = sizeof(bytes);
    sse_journal::StoredEvent stored_datagram = {};
    sse_journal::encode(datagram, short_config, &stored_datagram);
    const deepwin_market_data::StreamEvent decoded_datagram =
        sse_journal::decode(stored_datagram.event, stored_datagram.payload,
                            short_config);
    assert(decoded_datagram.kind == deepwin_market_data::kDatagramEvent);
    assert(decoded_datagram.size == sizeof(bytes));
    assert(std::memcmp(decoded_datagram.data, bytes, sizeof(bytes)) == 0);
    assert(decoded_datagram.hardware_ns == datagram.hardware_ns);
    assert(decoded_datagram.hardware_clock_index == 0);

    deepwin_market_data::StreamEvent v1_hardware = datagram;
    sse_journal::StoredEvent rejected_event = {};
    bool rejected_flags = false;
    try {
        sse_journal::encode(v1_hardware, plain_config, &rejected_event);
    } catch (const std::exception&) {
        rejected_flags = true;
    }
    assert(rejected_flags);

    std::remove(v1.c_str());
    std::remove(v1_bad_idle.c_str());
    std::remove(v2.c_str());
    std::remove(v2_no_hardware.c_str());
    std::remove(v2_old_payload.c_str());
    std::remove(v1_plain.c_str());
    std::cout << "sse_wait_transport_contract_test: PASS\n";
    return 0;
}
