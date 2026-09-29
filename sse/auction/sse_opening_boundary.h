#ifndef SSE_T0_OPENING_BOUNDARY_H
#define SSE_T0_OPENING_BOUNDARY_H

#include "sse/market_data/sse_primary_decoder.h"

#include <cstdint>
#include <map>
#include <string>

namespace sse_live {

struct OpeningBoundary {
    std::uint32_t channel_no;
    std::uint64_t opening_auction_last_app_seq;
    std::uint64_t status_app_seq;
    std::uint64_t opening_trade_rows;
    std::uint32_t opening_price_raw;
    std::uint64_t opening_quantity_raw;
    bool has_opening_trade;
    bool valid;
    std::string quality;
    OpeningBoundary();
};

class OpeningBoundaryTracker {
public:
    void observe(const TickEvent& event);
    bool get(const std::string& security_id, OpeningBoundary* output) const;
    std::size_t securities() const;

private:
    struct ChannelTradeState {
        std::uint64_t last_app_seq;
        std::uint64_t rows;
        std::uint32_t price_raw;
        std::uint64_t quantity_raw;
        bool multiple_prices;
        ChannelTradeState();
    };
    struct SecurityState {
        std::map<std::uint32_t, ChannelTradeState> trades;
        OpeningBoundary boundary;
        bool have_status;
        bool duplicate_status;
        SecurityState();
    };
    std::map<std::string, SecurityState> states_;
};

}  // namespace sse_live

#endif  // SSE_T0_OPENING_BOUNDARY_H
