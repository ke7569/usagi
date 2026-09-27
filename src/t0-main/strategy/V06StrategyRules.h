#ifndef USAGI_COMMON_STRATEGY_V06_STRATEGY_RULES_H
#define USAGI_COMMON_STRATEGY_V06_STRATEGY_RULES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace v06_strategy {

// The V06 B15/MH4 rules are deliberately independent of the OMS and market
// data adapters.  Runtime code supplies snapshots and consumes these values.

enum class Side {
    Buy,
    Sell
};

enum class OrderKind {
    Hit,
    Quote
};

enum class Signal {
    None,
    HitBuy,
    HitSell,
    QuoteBuy,
    QuoteSell
};

enum class ReservationStatus {
    Pending,
    Active,
    PartialFill,
    CancelPending,
    Filled,
    Cancelled,
    TimedOut
};

struct Config {
    // Values are in the same units as the approved deployment contract:
    // offset in permille, skew and global penalties in basis points.
    double offset_permille;
    double quote_ratio;
    double skewness_bps;
    double bias_factor;
    double position_limit_factor;
    double position_base_line;
    double tick_size;
    std::int64_t lot_size;
    std::int64_t hit_timeout_micros;
    double max_exposure;
    double max_global_skewness_bps;

    Config();
};

struct DynamicNewBiasMultipliers {
    double offset_multiplier;
    double bias_adjustment_factor;

    DynamicNewBiasMultipliers();
};

// exchange_time_micros may be an epoch timestamp or time-of-day.  The
// schedule uses the local time-of-day, matching the backtest implementation.
DynamicNewBiasMultipliers dynamicNewBiasMultipliers(std::int64_t exchange_time_micros);
double offsetMultiplier(std::int64_t exchange_time_micros);
double biasAdjustmentFactor(std::int64_t exchange_time_micros);
bool isStrategyTime(std::int64_t exchange_time_micros);
bool isExpansionTime(std::int64_t exchange_time_micros);

int strictAgreement(const std::array<double, 4>& heads);
int strictAgreement(const std::vector<double>& heads);

// requested is assumed to have passed the ordinary strategy quantity clamp.
// On a non-agreement side only the existing relative-position reduction is
// available; same-side reservations are retained until cancellation/fill
// confirmation and consume that reduction budget.
std::int64_t allowedVolume(Side side,
                           std::int64_t requested,
                           std::int64_t current_position,
                           std::int64_t reserved_same_side,
                           int agreement,
                           std::int64_t lot_size = 100);

// A pending order is revoked when the new MH4 gate cannot permit its complete
// remaining quantity.  reserved_before excludes this order and includes all
// earlier same-side reservations in deterministic order.
bool pendingOpeningNeedsCancel(Side side,
                               std::int64_t remaining,
                               std::int64_t current_position,
                               std::int64_t reserved_before,
                               int agreement,
                               std::int64_t lot_size = 100);

struct PricingInput {
    double prediction_permille;
    std::array<double, 4> agreement_heads;
    double bid_price;
    double ask_price;
    double last_price;
    std::int64_t bid_volume;
    std::int64_t ask_volume;
    std::int64_t exchange_time_micros;
    std::int64_t static_position;
    std::int64_t current_position;
    std::int64_t reserved_same_side;
    double global_buy_skew_bps;
    double global_sell_skew_bps;

    PricingInput();
};

struct PricingOutput {
    // valid means the market/prediction inputs produced finite prices.  A
    // valid result may still have Signal::None when no hit/quote is eligible.
    bool valid;
    int agreement;
    Signal signal;
    Side side;
    OrderKind kind;

    // speedbag_volume is the hit-margin/book-derived quantity.  requested is
    // that quantity after the relative position limit (including the 09:33
    // opening barrier).  allowed is requested after MH4 side gating.
    std::int64_t speedbag_volume;
    std::int64_t requested_volume;
    std::int64_t allowed_volume;
    std::int64_t timeout_micros;
    double order_price;

    double offset_multiplier;
    double bias_adjustment_factor;
    double offset;
    double quote_offset;
    double theo;
    double bias_multiplier;
    double effective_bias_factor;
    double unit_bias;
    double bias;
    double fixed_skew;
    double hit_buy_theo;
    double hit_sell_theo;
    double quote_buy_theo;
    double quote_sell_theo;
    double buy_margin;
    double sell_margin;
    double quote_buy_price;
    double quote_sell_price;

    PricingOutput();
};

// A direct copy of the ordinary position-limit part of SpeedBag.  It does not
// impose a global exposure cap; max_exposure only controls directional skew.
std::int64_t positionLimitedVolume(Side side,
                                   std::int64_t requested,
                                   std::int64_t current_position,
                                   std::int64_t static_position,
                                   std::int64_t exchange_time_micros,
                                   double position_limit_factor = 1.0,
                                   std::int64_t lot_size = 100);

class V06StrategyRules {
public:
    explicit V06StrategyRules(const Config& config = Config());

    const Config& config() const;
    PricingOutput price(const PricingInput& input) const;

    std::int64_t positionLimitedVolume(Side side,
                                       std::int64_t requested,
                                       std::int64_t current_position,
                                       std::int64_t static_position,
                                       std::int64_t exchange_time_micros) const;

private:
    Config config_;
};

struct PositionClampInput {
    std::int64_t requested;
    Side side;
    OrderKind kind;
    std::int64_t current_position;
    std::int64_t static_position;
    // Absolute shares held at the session opening (static plus the carried
    // relative deviation), used for the A-share T+1 sell budget.
    std::int64_t opening_position;
    std::int64_t long_position;
    std::int64_t short_position;
    std::int64_t dirty_buy_hit;
    std::int64_t dirty_buy_quote;
    std::int64_t dirty_sell_hit;
    std::int64_t dirty_sell_quote;
    std::int64_t exchange_time_micros;

    PositionClampInput();
};

// Full ordinary SpeedBag quantity clamp, including daily gross buy/sell
// budgets and outstanding-order dirty quantities.  This is separate from the
// MH4 gate so callers can preserve their existing execution constraints.
std::int64_t ordinaryAllowedVolume(const PositionClampInput& input,
                                   const Config& config = Config());

struct GlobalExposureInstrument {
    std::int64_t static_position_after_adjustment;
    std::int64_t position_after_adjustment;
    double open_price;

    GlobalExposureInstrument();
};

struct GlobalExposureInput {
    std::vector<GlobalExposureInstrument> instruments;
    // Account-wide cumulative net cash spent at this synchronized 10-second
    // snapshot. Buy cash is positive and sell cash is negative.
    double net_traded_amount;

    GlobalExposureInput();
};

struct GlobalExposureResult {
    bool valid;
    double base_value;
    double initial_net;
    double net_traded_amount;
    double exposure;

    GlobalExposureResult();
};

GlobalExposureResult globalExposure(const GlobalExposureInput& input);

struct DirectionalSkew {
    bool valid;
    double buy_bps;
    double sell_bps;

    DirectionalSkew();
};

DirectionalSkew directionalSkew(double exposure,
                                double max_exposure = 0.05,
                                double max_skewness_bps = 10.0);
DirectionalSkew directionalSkew(const GlobalExposureResult& exposure,
                                const Config& config = Config());

bool reservationStillHeld(ReservationStatus status);
bool reservationTerminal(ReservationStatus status);

struct Reservation {
    std::int64_t order_id;
    Side side;
    OrderKind kind;
    std::int64_t remaining;
    ReservationStatus status;
    bool opening;

    Reservation();
};

// Local deterministic reservation state.  It has no OMS/network dependency;
// a runtime can mirror its order callbacks into this ledger.  Cancel requests
// deliberately leave remaining quantity reserved until a terminal update.
class ReservationBook {
public:
    bool reserve(std::int64_t order_id,
                 Side side,
                 OrderKind kind,
                 std::int64_t volume,
                 bool opening = true);
    bool requestCancel(std::int64_t order_id);
    bool update(std::int64_t order_id,
                ReservationStatus status,
                std::int64_t remaining);
    bool contains(std::int64_t order_id) const;

    std::int64_t reservedVolume(Side side) const;
    std::int64_t reservedVolume(Side side, OrderKind kind) const;
    std::vector<std::int64_t> openingOrdersToRevoke(Side side,
                                                    std::int64_t current_position,
                                                    int agreement,
                                                    std::int64_t lot_size = 100) const;
    std::vector<Reservation> reservations() const;

private:
    std::map<std::int64_t, Reservation> reservations_;
};

}  // namespace v06_strategy

#endif  // USAGI_COMMON_STRATEGY_V06_STRATEGY_RULES_H
