#include "sse_opening_boundary.h"

namespace sse_live {
namespace {

const std::uint64_t kAuctionEndMicros = (9ULL * 3600ULL + 25ULL * 60ULL) * 1000000ULL;
const std::uint64_t kContinuousStartMicros = (9ULL * 3600ULL + 30ULL * 60ULL) * 1000000ULL;

bool in_opening_batch(const TickEvent& event) {
    return event.time_of_day_micros >= kAuctionEndMicros &&
           event.time_of_day_micros < kContinuousStartMicros;
}

bool zero_status(const TickEvent& event) {
    return event.event_type == 'S' && event.buy_order_no == 0U &&
           event.sell_order_no == 0U && event.price_raw == 0U &&
           event.quantity_raw == 0U && event.amount_raw == 0U &&
           static_cast<unsigned char>(event.side) == 3U;
}

}  // namespace

OpeningBoundary::OpeningBoundary()
    : channel_no(0), opening_auction_last_app_seq(0), status_app_seq(0),
      opening_trade_rows(0), opening_price_raw(0), opening_quantity_raw(0),
      has_opening_trade(false), valid(false), quality("status_missing") {}

OpeningBoundaryTracker::ChannelTradeState::ChannelTradeState()
    : last_app_seq(0), rows(0), price_raw(0), quantity_raw(0),
      multiple_prices(false) {}

OpeningBoundaryTracker::SecurityState::SecurityState()
    : have_status(false), duplicate_status(false) {}

void OpeningBoundaryTracker::observe(const TickEvent& event) {
    if (!in_opening_batch(event)) return;
    SecurityState& state = states_[event.security_id];
    if (event.event_type == 'T') {
        ChannelTradeState& trades = state.trades[event.channel_no];
        if (trades.rows != 0U && trades.price_raw != event.price_raw)
            trades.multiple_prices = true;
        trades.last_app_seq = event.app_seq_num;
        trades.rows += 1U;
        trades.price_raw = event.price_raw;
        trades.quantity_raw += event.quantity_raw;
        return;
    }
    if (event.event_type != 'S') return;
    if (state.have_status) state.duplicate_status = true;
    state.have_status = true;
    OpeningBoundary& boundary = state.boundary;
    boundary.channel_no = event.channel_no;
    boundary.status_app_seq = event.app_seq_num;
    boundary.opening_auction_last_app_seq =
        event.app_seq_num == 0U ? 0U : event.app_seq_num - 1U;
    std::map<std::uint32_t, ChannelTradeState>::const_iterator found =
        state.trades.find(event.channel_no);
    if (found != state.trades.end()) {
        boundary.has_opening_trade = found->second.rows != 0U;
        boundary.opening_trade_rows = found->second.rows;
        boundary.opening_price_raw = found->second.price_raw;
        boundary.opening_quantity_raw = found->second.quantity_raw;
    }
    if (!zero_status(event)) boundary.quality = "invalid_status_payload";
    else if (state.duplicate_status) boundary.quality = "duplicate_status";
    else if (event.app_seq_num == 0U) boundary.quality = "invalid_status_app_seq";
    else if (found == state.trades.end() || found->second.rows == 0U) {
        boundary.valid = true;
        boundary.quality = "no_opening_trade";
    } else if (found->second.multiple_prices) boundary.quality = "multiple_opening_prices";
    else if (found->second.last_app_seq + 1U != event.app_seq_num)
        boundary.quality = "status_not_adjacent_to_last_trade";
    else {
        boundary.valid = true;
        boundary.quality = "ok";
    }
}

bool OpeningBoundaryTracker::get(const std::string& security_id,
                                 OpeningBoundary* output) const {
    if (!output) return false;
    std::map<std::string, SecurityState>::const_iterator found = states_.find(security_id);
    if (found == states_.end() || !found->second.have_status) return false;
    *output = found->second.boundary;
    return true;
}

std::size_t OpeningBoundaryTracker::securities() const { return states_.size(); }

}  // namespace sse_live
