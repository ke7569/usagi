#ifndef SSE_T0_AUCTION_STATIC_METADATA_H
#define SSE_T0_AUCTION_STATIC_METADATA_H

#include "sse/market_data/sse_primary_decoder.h"

#include <cstdint>
#include <map>
#include <string>

namespace sse_auction59 {

struct StaticMetadata {
    std::uint32_t date;
    double pre_close;
    double upper_limit;
    double lower_limit;
    std::uint32_t listing_date;
    bool is_ipo_first_day;
    bool limits_valid;
    std::string source;
    std::string quality;
    StaticMetadata();
    bool auction_static_ready() const;
};

typedef std::map<std::string, StaticMetadata> StaticMetadataMap;

// Auction static metadata is loaded from the operator-prepared per-day CSV
// (configs/staging/<date>/generated/sse_static_<date>.csv, produced by
// prepare_sse_static_metadata.sh) whose explicit flags mirror what the engine
// needs. The JSON daily-config loader from the live tree was dropped because it
// was unused in production and could not derive limits_valid consistently.
bool load_static_metadata_csv(const std::string& path, std::uint32_t expected_date,
                              StaticMetadataMap* output, std::string* error = 0);
bool reconcile_snapshot_pre_close(const sse_live::Snapshot& snapshot,
                                  const StaticMetadataMap& metadata,
                                  std::string* quality = 0);

}  // namespace sse_auction59

#endif  // SSE_T0_AUCTION_STATIC_METADATA_H
