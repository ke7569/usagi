#ifndef SSE_T0_TICK_STATIC_METADATA_H
#define SSE_T0_TICK_STATIC_METADATA_H

#include <cstdint>
#include <map>
#include <string>

namespace sse_tick {

// The v0.4 stock_day sidecar stores these values as Float64 columns.  The
// native tick path keeps presence flags separate from the numeric values so a
// missing column/blank cell can never become a synthetic 1.0.
struct DailyStaticMetadata {
    std::uint32_t date;
    std::uint32_t prev_trade_date;
    double prev_turnover;
    double avg_amount;
    double turnover_threshold;
    double free_share;
    double pre_close;
    double limit_price;
    double stop_price;
    bool has_date;
    bool has_prev_trade_date;
    bool has_prev_turnover;
    bool has_avg_amount;
    bool has_turnover_threshold;
    bool has_free_share;
    bool has_pre_close;
    bool has_limit_price;
    bool has_stop_price;
    std::string threshold_basis;
    std::string source;
    std::string quality;

    DailyStaticMetadata();

    // A row is formal-model ready only when every static input used by the
    // daily contract is present and numerically valid.  The flags are
    // intentionally checked in addition to the values.
    bool complete() const;
};

typedef std::map<std::string, DailyStaticMetadata> DailyStaticMetadataMap;

// Normalize 600000.SH, SH.600000, or 600000 to the decoder's six-digit SSE
// security id.  SZE ids and malformed identifiers are rejected.
bool normalize_sse_security_id(const std::string& value, std::string* output);

// Load a daily metadata CSV.  The canonical v0.4 stock_day columns are
// accepted, as are the existing SSE aliases security_id/upper_limit/lower_limit
// and a minimal free_share CSV for diagnostic/backward-compatible replay.
// expected_date==0 derives one date from the file and rejects mixed dates.
bool load_daily_static_metadata_csv(const std::string& path,
                                    std::uint32_t expected_date,
                                    DailyStaticMetadataMap* output,
                                    std::string* error = 0);

// Load either the normalized JSON contract emitted by
// deploy/prepare_sse_static_json.py or the authoritative research-machine
// config_sse_daily JSON.  For the latter, turnover_threshold is derived from
// HistoryAmount and prev_turnover is intentionally unused by the tick model.
bool load_daily_static_metadata_json(const std::string& path,
                                     std::uint32_t expected_date,
                                     DailyStaticMetadataMap* output,
                                     std::string* error = 0);

// C++11 deliberately does not claim to parse Arrow IPC.  A .arrow path
// returns false with a conversion command hint; CSV remains available only
// for backward-compatible callers.
bool load_daily_static_metadata(const std::string& path,
                                std::uint32_t expected_date,
                                DailyStaticMetadataMap* output,
                                std::string* error = 0);

}  // namespace sse_tick

#endif  // SSE_T0_TICK_STATIC_METADATA_H
