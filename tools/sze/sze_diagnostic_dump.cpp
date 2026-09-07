#include "sze/market_data/SZEProtocol.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {

bool read_exact(std::ifstream* input, void* destination, std::size_t size)
{
    input->read(static_cast<char*>(destination), size);
    return static_cast<std::size_t>(input->gcount()) == size;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: sze_diagnostic_dump FILE\n";
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary);
    sze_md::DiagnosticFileHeader header;
    std::memset(&header, 0, sizeof(header));
    if (!input.is_open() || !read_exact(&input, &header, sizeof(header))) {
        std::cerr << "sze_diagnostic_dump status=FAIL reason=read-header\n";
        return 1;
    }
    if (header.magic != sze_md::kDiagnosticFileMagic ||
        header.version != sze_md::kDiagnosticFileVersion ||
        header.header_size != sizeof(sze_md::DiagnosticFileHeader) ||
        header.record_size != sizeof(sze_md::DiagnosticRecord) ||
        header.committed_records > header.capacity) {
        std::cerr << "sze_diagnostic_dump status=FAIL reason=invalid-header\n";
        return 1;
    }

    char build_id[sizeof(header.build_id) + 1U];
    std::memcpy(build_id, header.build_id, sizeof(header.build_id));
    build_id[sizeof(header.build_id)] = '\0';
    std::cout << "sze_diagnostic_dump status=PASS"
              << " version=" << header.version
              << " trading_day=" << header.trading_day
              << " source_id=" << header.source_id
              << " capacity=" << header.capacity
              << " committed_records=" << header.committed_records
              << " dropped_records=" << header.dropped_records
              << " build_id=" << build_id << '\n';
    std::cout << "index,diagnostic_realtime_ns,receive_mono_ns,channel_index,"
                 "packet_number,record_offset,record_length,datagram_length,"
                 "decode_status_code,decode_status,failure_code,failure,"
                 "invalidating,message_type,feed_sequence,channel_number,"
                 "channel_sequence,exchange_time,captured_length,truncated,"
                 "payload_hex\n";

    for (std::uint64_t index = 0; index < header.committed_records; ++index) {
        sze_md::DiagnosticRecord record;
        std::memset(&record, 0, sizeof(record));
        if (!read_exact(&input, &record, sizeof(record)) ||
            record.commit != sze_md::kDiagnosticRecordCommit ||
            record.captured_length > sze_md::kDiagnosticPayloadBytes) {
            std::cerr << "sze_diagnostic_dump status=FAIL reason=invalid-record"
                      << " index=" << index << '\n';
            return 1;
        }
        const sze_md::DecodeStatus status =
            static_cast<sze_md::DecodeStatus>(record.decode_status);
        const sze_md::DecodeFailureReason failure =
            static_cast<sze_md::DecodeFailureReason>(record.failure_reason);
        const bool non_invalidating =
            (record.flags & sze_md::kDiagnosticNonInvalidating) != 0U;
        const bool truncated =
            (record.flags & sze_md::kDiagnosticPayloadTruncated) != 0U;
        std::cout << index << ',' << record.diagnostic_realtime_ns << ','
                  << record.receive_mono_ns << ',' << record.channel_index << ','
                  << record.packet_number << ',' << record.record_offset << ','
                  << record.record_length << ',' << record.datagram_length << ','
                  << static_cast<unsigned int>(record.decode_status) << ','
                  << sze_md::decode_status_name(status) << ','
                  << static_cast<unsigned int>(record.failure_reason) << ','
                  << sze_md::decode_failure_reason_name(failure) << ','
                  << (non_invalidating ? 0 : 1) << ','
                  << static_cast<unsigned int>(record.message_type) << ','
                  << record.feed_sequence << ',' << record.channel_number << ','
                  << record.channel_sequence << ',' << record.exchange_time << ','
                  << record.captured_length << ',' << (truncated ? 1 : 0) << ',';
        const std::size_t payload_size = std::min(
            static_cast<std::size_t>(record.captured_length),
            static_cast<std::size_t>(sze_md::kDiagnosticPayloadBytes));
        for (std::size_t byte = 0; byte < payload_size; ++byte) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(record.payload[byte]);
        }
        std::cout << std::dec << std::setfill(' ') << '\n';
    }
    return 0;
}
