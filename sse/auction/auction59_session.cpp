#include "sse/auction/auction59_session.h"

#include <cmath>
#include <limits>

namespace sse_auction59 {
namespace {

const std::uint64_t kAuctionStart = (9ULL * 3600ULL + 15ULL * 60ULL) * 1000000ULL;
const std::uint64_t kAuctionEnd = (9ULL * 3600ULL + 25ULL * 60ULL) * 1000000ULL;
const std::uint64_t kContinuousStart = (9ULL * 3600ULL + 30ULL * 60ULL) * 1000000ULL;
const double kPriceTolerance = 0.0005;

bool valid_price(double value) {
    return std::isfinite(value) && value > 0.0 &&
           value * 1000.0 <= std::numeric_limits<std::int32_t>::max();
}

bool old_ipo_limits(const StaticMetadata& metadata) {
    // Match the engine's integer, cent-rounded 64%/144% special case.
    const std::int64_t price = static_cast<std::int64_t>(
        std::floor(metadata.pre_close * 1000.0 + 0.5));
    const double lower = ((price * 64 + 500) / 1000 * 10) / 1000.0;
    const double upper = ((price * 144 + 500) / 1000 * 10) / 1000.0;
    return std::fabs(metadata.lower_limit - lower) <= kPriceTolerance &&
           std::fabs(metadata.upper_limit - upper) <= kPriceTolerance;
}

std::string engine_reason(const AuctionResult& result) {
    std::string message("auction_invalid");
    for (std::size_t bit = 0; bit <= kNumericOverflow; ++bit) {
        if (result.quality_reason_mask & (1ULL << bit)) {
            message += ':';
            message += quality_reason_name(bit);
        }
    }
    return message;
}

}  // namespace

Session::Session(const std::string& security_id, const StaticMetadata& metadata,
                 bool ipo_known)
    : security_id_(security_id), metadata_(metadata), snapshot_(),
      have_snapshot_(false), have_boundary_(false), ready_(false),
      failed_(false), shared_static_valid_(true), reason_("awaiting_opening_status") {
    if (!metadata.auction_static_ready() || !valid_price(metadata.pre_close) ||
        !valid_price(metadata.lower_limit) || !valid_price(metadata.upper_limit)) {
        fail_static("static_metadata_invalid");
        return;
    }
    if (!ipo_known && old_ipo_limits(metadata)) {
        disable("ipo_first_day_unknown");
        return;
    }
    accumulator_.reset(new AuctionAccumulator(security_id, metadata));
}

void Session::fail_static(const std::string& reason) {
    shared_static_valid_ = false;
    disable(reason);
}

void Session::disable(const std::string& reason) {
    ready_ = false;
    failed_ = true;
    reason_ = reason;
    accumulator_.reset();
    factors_.clear();
}

void Session::seal_missing() {
    if (!ready_ && !failed_) disable("auction_not_ready_at_first_snapshot:" + reason_);
}

void Session::on_tick(const sse_live::TickEvent& event, std::uint64_t realtime_ns,
                       std::uint64_t arrival_index) {
    if (failed_ || ready_ || event.security_id != security_id_ ||
        event.time_of_day_micros < kAuctionStart ||
        event.time_of_day_micros >= kContinuousStart) return;
    // BoundaryTracker validates S payload, channel, sequence adjacency and
    // the opening trades. Never synthesize S from a clock or a snapshot.
    boundary_tracker_.observe(event);
    accumulator_->observe(event, realtime_ns, arrival_index);
    if (event.event_type == 'S' && boundary_tracker_.get(security_id_, &boundary_)) {
        have_boundary_ = true;
        if (!boundary_.valid) {
            disable("opening_boundary:" + boundary_.quality);
            return;
        }
        if (!boundary_.has_opening_trade) {
            disable("opening_boundary:no_opening_trade");
            return;
        }
    }
    try_ready();
}

void Session::on_snapshot(const sse_live::Snapshot& snapshot) {
    if (snapshot.security_id != security_id_ ||
        snapshot.time_of_day_micros < kAuctionEnd) return;
    // Continue this check even after readiness or a local auction failure:
    // stale daily metadata also invalidates tick-model trading for this stock.
    if (!valid_price(snapshot.pre_close_price)) {
        fail_static("snapshot_pre_close_invalid");
        return;
    }
    if (std::fabs(snapshot.pre_close_price - metadata_.pre_close) > kPriceTolerance) {
        fail_static("snapshot_pre_close_conflict");
        return;
    }
    if (valid_price(snapshot.open_price) &&
        (snapshot.open_price < metadata_.lower_limit - kPriceTolerance ||
         snapshot.open_price > metadata_.upper_limit + kPriceTolerance)) {
        fail_static("snapshot_open_outside_daily_limits");
        return;
    }
    if (failed_ || ready_) return;
    if (have_snapshot_ && snapshot.time_of_day_micros < snapshot_.time_of_day_micros) return;
    snapshot_ = snapshot;
    have_snapshot_ = true;
    try_ready();
}

void Session::try_ready() {
    if (failed_ || ready_) return;
    if (!have_boundary_) {
        reason_ = "awaiting_opening_status";
        return;
    }
    if (!have_snapshot_) {
        reason_ = "awaiting_opening_snapshot";
        return;
    }
    // A 09:25 snapshot can precede the full opening batch. Wait for its next
    // update instead of accepting incomplete volume or failing prematurely.
    if (snapshot_.time_of_day_micros < kContinuousStart) {
        if (snapshot_.volume < 0 || boundary_.opening_quantity_raw % 1000ULL != 0) {
            disable("snapshot_auction_volume_invalid");
            return;
        }
        const std::uint64_t shares = boundary_.opening_quantity_raw / 1000ULL;
        if (static_cast<std::uint64_t>(snapshot_.volume) < shares) {
            reason_ = "awaiting_complete_auction_snapshot";
            return;
        }
        if (static_cast<std::uint64_t>(snapshot_.volume) != shares) {
            disable("snapshot_auction_volume_conflict");
            return;
        }
    }
    if (!valid_price(snapshot_.open_price)) {
        reason_ = "awaiting_snapshot_open_price";
        return;
    }
    const double opening_price = boundary_.opening_price_raw / 1000.0;
    if (std::fabs(snapshot_.open_price - opening_price) > kPriceTolerance) {
        disable("snapshot_open_price_conflict");
        return;
    }
    const AuctionResult result = accumulator_->finalize();
    if (!result.hard_valid) {
        disable(engine_reason(result));
        return;
    }
    const std::uint64_t complete_mask = (1ULL << kAuction59FactorCount) - 1ULL;
    if (result.projected_valid_mask != complete_mask) {
        disable("auction_factors_incomplete");
        return;
    }
    for (std::size_t i = 0; i < kAuction59FactorCount; ++i) {
        if (!std::isfinite(result.factors[i])) {
            disable("auction_factors_nonfinite");
            return;
        }
    }
    factors_.assign(result.factors, result.factors + kAuction59FactorCount);
    accumulator_.reset();
    ready_ = true;
    reason_ = "ok";
}

}  // namespace sse_auction59
