#include "sse/market_data/sse_primary_decoder.h"
#include "sse/auction/auction_static_metadata.h"

#include <glob.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

std::vector<std::string> record_files(const std::string& root) {
    glob_t matches;
    std::memset(&matches, 0, sizeof(matches));
    std::vector<std::string> result;
    const std::string pattern = root + "/channel_*/records.bin";
    if (glob(pattern.c_str(), 0, 0, &matches) == 0) {
        for (std::size_t i = 0; i < matches.gl_pathc; ++i)
            result.push_back(matches.gl_pathv[i]);
    }
    globfree(&matches);
    return result;
}

bool scan(const std::string& path, std::map<std::string, sse_live::Snapshot>* first,
          std::uint64_t* invalid) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return false;
    while (true) {
        sse_live::DiskRecordHeader header;
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (input.gcount() == 0) break;
        if (input.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
            header.magic != sse_live::kDiskRecordMagic || header.version != 1U ||
            header.header_size != sizeof(header)) return false;
        std::vector<unsigned char> payload(header.payload_length);
        input.read(reinterpret_cast<char*>(&payload[0]), payload.size());
        if (input.gcount() != static_cast<std::streamsize>(payload.size())) return false;
        sse_live::Snapshot snapshot;
        std::string error;
        if (!sse_live::decode_primary_snapshot(&payload[0], payload.size(), &snapshot, &error)) {
            ++*invalid; continue;
        }
        if (first->find(snapshot.security_id) == first->end())
            (*first)[snapshot.security_id] = snapshot;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: " << argv[0]
                  << " SNAPSHOT_ROOT STATIC_CSV YYYYMMDD OUTPUT_JSON\n";
        return 2;
    }
    const std::uint32_t date = static_cast<std::uint32_t>(std::strtoul(argv[3], 0, 10));
    sse_auction59::StaticMetadataMap metadata;
    std::string error;
    if (!sse_auction59::load_static_metadata_csv(argv[2], date, &metadata, &error)) {
        std::cerr << error << "\n"; return 2;
    }
    const std::vector<std::string> paths = record_files(argv[1]);
    if (paths.empty()) { std::cerr << "no Snapshot files\n"; return 2; }
    std::map<std::string, sse_live::Snapshot> first;
    std::uint64_t invalid = 0U;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!scan(paths[i], &first, &invalid)) {
            std::cerr << "failed to scan " << paths[i] << "\n"; return 2;
        }
    }
    std::size_t static_missing = 0U, pre_close_conflict = 0U, ready = 0U,
                explicit_limits_missing = 0U;
    std::vector<std::string> conflict_samples;
    for (std::map<std::string, sse_live::Snapshot>::const_iterator it = first.begin();
         it != first.end(); ++it) {
        sse_auction59::StaticMetadataMap::const_iterator found = metadata.find(it->first);
        if (found == metadata.end()) { ++static_missing; continue; }
        std::string quality;
        if (sse_auction59::reconcile_snapshot_pre_close(it->second, metadata, &quality)) ++ready;
        else if (quality == "pre_close_conflict") {
            ++pre_close_conflict;
            if (conflict_samples.size() < 10U) conflict_samples.push_back(it->first);
        } else if (quality == "explicit_limits_missing") ++explicit_limits_missing;
    }
    std::ofstream output(argv[4]);
    if (!output) { std::cerr << "cannot write output JSON\n"; return 2; }
    output << "{\n"
           << "  \"date\": " << date << ",\n"
           << "  \"snapshot_securities\": " << first.size() << ",\n"
           << "  \"static_rows\": " << metadata.size() << ",\n"
           << "  \"auction_static_ready\": " << ready << ",\n"
           << "  \"explicit_limits_missing\": " << explicit_limits_missing << ",\n"
           << "  \"static_security_missing\": " << static_missing << ",\n"
           << "  \"pre_close_conflict\": " << pre_close_conflict << ",\n"
           << "  \"invalid_snapshot_records\": " << invalid << ",\n"
           << "  \"conflict_samples\": [";
    for (std::size_t i = 0; i < conflict_samples.size(); ++i) {
        if (i) output << ',';
        output << '"' << conflict_samples[i] << '"';
    }
    output << "]\n}\n";
    std::cout << "snapshot_static_audit: snapshots=" << first.size()
              << " ready=" << ready << " no_limits=" << explicit_limits_missing
              << " missing=" << static_missing << " conflicts=" << pre_close_conflict << "\n";
    // An unknown new listing is isolated by the live worker. A conflict for a
    // known security invalidates the complete static file.
    return (invalid != 0U || pre_close_conflict != 0U) ? 1 : 0;
}
