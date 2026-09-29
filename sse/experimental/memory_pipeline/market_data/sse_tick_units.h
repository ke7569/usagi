#ifndef SSE_T0_TICK_UNITS_H
#define SSE_T0_TICK_UNITS_H

// Canonical model-unit contract for the SSE tick pipeline.
//
// EFH sse_hpf feeds carry integer "raw" values that are NOT the model units:
//   raw price    = yuan * 1000          -> model price in yuan
//   raw quantity = shares * 1000        -> model quantity in shares
// EFH snapshot fields are decoded straight into canonical units in
// sse_primary_decoder.cpp (yuan / shares), so snapshots need no extra scale.
//
// Rule: every raw->canonical conversion happens through the helpers below,
// applied at the single adapter boundary where an EFH tick enters the model
// pipeline (OrderBook::apply).  Order book levels, flow windows, factor code
// and the sampling gate only ever see canonical shares/yuan.
#include <cstdint>

namespace sse_tick {

// EFH tick price scale: raw price / 1000 = yuan.
static const std::uint32_t kEfhPriceScale = 1000U;
// EFH tick quantity scale: raw quantity / 1000 = shares (100 shares arrive
// as 100000).
static const std::uint64_t kEfhQuantityScale = 1000ULL;
// Canonical sampling gate: a batch must contain at least 100 shares of real
// trades before it may produce a sample point.
static const std::uint64_t kMinBatchTradeShares = 100ULL;
// A per-security quiet period (NIC arrival time) of 100us settles a batch.
static const std::uint64_t kBatchQuietMicros = 100ULL;

inline std::uint64_t to_shares(std::uint64_t efh_quantity_raw) {
    return efh_quantity_raw / kEfhQuantityScale;
}

inline double price_yuan(std::uint32_t efh_price_raw) {
    return static_cast<double>(efh_price_raw) / static_cast<double>(kEfhPriceScale);
}

}  // namespace sse_tick

#endif  // SSE_T0_TICK_UNITS_H
