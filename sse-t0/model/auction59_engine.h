#ifndef SSE_T0_AUCTION59_ENGINE_H
#define SSE_T0_AUCTION59_ENGINE_H

#include "auction_static_metadata.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sse_auction59 {

static const std::size_t kCanonicalFactorCount = 62U;
static const std::size_t kAuction59FactorCount = 59U;

enum Side { kBuy = 0, kSell = 1 };

enum QualityReason {
    kSuspended = 0,
    kSourceMissing = 1,
    kStaticDataMissing = 2,
    kMetaConflict = 3,
    kOpeningBoundaryConflict = 4,
    kUnknownOrderType = 5,
    kDuplicateOrNoncausalSeq = 6,
    kAbnormalCancel = 7,
    kAbnormalTrade = 8,
    kOrderConservation = 9,
    kNoAuctionMatch = 10,
    kMultipleOpeningPrices = 11,
    kClearingTieUnresolved = 12,
    kReconstructedPriceMismatch = 13,
    kReconstructedQtyMismatch = 14,
    kPathCoverage = 15,
    kOneSidedResidualBook = 16,
    kPriceBoundary = 17,
    kCounterfactualUnreachable = 18,
    kNumericOverflow = 19
};

struct AuctionResult {
    std::string security_id;
    bool source_present;
    bool has_status;
    bool has_auction_match;
    bool hard_valid;
    std::uint32_t status_channel_no;
    std::uint64_t opening_auction_last_app_seq;
    std::uint64_t quality_reason_mask;
    std::uint64_t canonical_valid_mask;
    std::uint64_t projected_valid_mask;
    std::uint64_t source_order_rows;
    std::uint64_t source_trade_rows;
    std::uint64_t conservation_error_count;
    std::int32_t pre_close_price_tick;
    std::int32_t lower_limit_tick;
    std::int32_t upper_limit_tick;
    std::int32_t observed_auction_price_tick;
    std::int64_t observed_auction_qty;
    std::int32_t reconstructed_auction_price_tick;
    std::int64_t reconstructed_auction_qty;
    float valid_price_share_full;
    float valid_price_share_late;
    float canonical_factors[kCanonicalFactorCount];
    float factors[kAuction59FactorCount];

    AuctionResult();
};

class AuctionAccumulator {
public:
    AuctionAccumulator(const std::string& security_id,
                       const StaticMetadata& metadata);

    void observe(const sse_live::TickEvent& event,
                 std::uint64_t realtime_ns,
                 std::uint64_t arrival_index);
    bool has_status() const;
    bool finalized() const;
    AuctionResult finalize();

private:
    struct Impl;
    Impl* impl_;
    AuctionAccumulator(const AuctionAccumulator&);
    AuctionAccumulator& operator=(const AuctionAccumulator&);

public:
    ~AuctionAccumulator();
};

const char* quality_reason_name(std::size_t bit);
const char* auction59_factor_name(std::size_t bit);

}  // namespace sse_auction59

#endif  // SSE_T0_AUCTION59_ENGINE_H
