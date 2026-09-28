#include "common/strategy/V06StrategyRules.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace v06_strategy {
namespace {

const std::int64_t kMicrosPerMinute = 60LL * 1000LL * 1000LL;
const std::int64_t kMicrosPerHour = 60LL * kMicrosPerMinute;
const std::int64_t kMicrosPerDay = 24LL * kMicrosPerHour;
const std::int64_t kStrategyBegin = 9LL * kMicrosPerHour + 30LL * kMicrosPerMinute;
const std::int64_t kPositionLimitZeroUntil =
    9LL * kMicrosPerHour + 33LL * kMicrosPerMinute;
// The continuous session ends at 14:56.  The old SpeedBag path does not
// submit orders during the closing auction.
const std::int64_t kClosingAuctionStart =
    14LL * kMicrosPerHour + 56LL * kMicrosPerMinute;

double quietNaN() {
    return std::numeric_limits<double>::quiet_NaN();
}

bool finitePositive(double value) {
    return std::isfinite(value) && value > 0.0;
}

std::int64_t timeOfDay(std::int64_t exchange_time_micros) {
    std::int64_t value = exchange_time_micros % kMicrosPerDay;
    if (value < 0) {
        value += kMicrosPerDay;
    }
    return value;
}

std::int64_t floorNonnegative(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    const double max_value = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    if (value >= max_value) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::floor(value));
}

std::int64_t floorToLot(std::int64_t value, std::int64_t lot_size) {
    if (value <= 0 || lot_size <= 0) {
        return 0;
    }
    return value - value % lot_size;
}

std::int64_t clampLongDoubleToInt64(long double value) {
    if (!std::isfinite(static_cast<double>(value)) || value <= 0.0L) {
        return 0;
    }
    const long double max_value =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (value >= max_value) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

std::int64_t positiveRequested(std::int64_t value) {
    return value > 0 ? value : 0;
}

std::int64_t effectiveLot(std::int64_t lot_size) {
    return lot_size > 0 ? lot_size : 100;
}

std::int64_t sideSign(Side side) {
    return side == Side::Buy ? 1 : -1;
}

std::int64_t reductionCapacity(Side side, std::int64_t current_position) {
    if (side == Side::Buy) {
        // Avoid overflowing when current_position is INT64_MIN.
        if (current_position >= 0) {
            return 0;
        }
        if (current_position == std::numeric_limits<std::int64_t>::min()) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return -current_position;
    }
    return current_position > 0 ? current_position : 0;
}

std::int64_t rawHitVolume(double bias_factor,
                          double unit_bias,
                          double margin,
                          std::int64_t book_volume,
                          std::int64_t lot_size) {
    const std::int64_t lot = effectiveLot(lot_size);
    const std::int64_t level_volume = floorToLot(positiveRequested(book_volume), lot);
    if (bias_factor >= 1.0e-6 && unit_bias > 0.0 && std::isfinite(margin)) {
        const std::int64_t by_margin = floorNonnegative(margin / unit_bias);
        return std::min(level_volume, by_margin);
    }
    return level_volume;
}

std::int64_t sumNonnegative(std::int64_t first, std::int64_t second) {
    const long double sum = static_cast<long double>(positiveRequested(first)) +
                            static_cast<long double>(positiveRequested(second));
    return clampLongDoubleToInt64(sum);
}

}  // namespace

Config::Config()
    : offset_permille(1.0),
      quote_ratio(10.0),
      skewness_bps(1.0),
      bias_factor(0.3),
      position_limit_factor(1.0),
      position_base_line(500000.0),
      tick_size(0.01),
      lot_size(100),
      hit_timeout_micros(1000000),
      max_exposure(0.05),
      max_global_skewness_bps(10.0) {}

DynamicNewBiasMultipliers::DynamicNewBiasMultipliers()
    : offset_multiplier(1.0), bias_adjustment_factor(1.0) {}

DynamicNewBiasMultipliers dynamicNewBiasMultipliers(std::int64_t exchange_time_micros) {
    const std::int64_t tod = timeOfDay(exchange_time_micros);
    const std::int64_t hour = tod / kMicrosPerHour;
    const std::int64_t minute = (tod % kMicrosPerHour) / kMicrosPerMinute;

    double offset = 1.0;
    if (hour == 9 && minute >= 25 && minute <= 30) {
        offset = 10.0;
    } else if (hour == 9 && minute == 31) {
        offset = 5.0;
    } else if (hour == 9 && minute == 32) {
        offset = 4.0;
    } else if (hour == 9 && minute == 33) {
        offset = 3.0;
    } else if (hour == 9 && minute == 34) {
        offset = 2.0;
    } else if (hour == 9 && minute == 35) {
        offset = 1.8;
    } else if (hour == 9 && minute == 36) {
        offset = 1.6;
    } else if (hour == 9 && minute == 37) {
        offset = 1.4;
    } else if (hour == 9 && minute == 38) {
        offset = 1.2;
    } else if (hour == 9 && minute == 39) {
        offset = 1.1;
    }

    // This is the existing DynamicNewBias close/settlement schedule.  Keep
    // the ratios explicit: the base bias factor remains 0.3.
    double bias = 1.0;
    if (hour == 13 && minute < 30) {
        bias = 0.4 / 0.3;
    } else if (hour == 13) {
        bias = 0.5 / 0.3;
    } else if (hour == 14 && minute < 30) {
        bias = 0.7 / 0.3;
    } else if (hour == 14) {
        bias = 1.0 / 0.3;
    }

    DynamicNewBiasMultipliers result;
    result.offset_multiplier = offset;
    result.bias_adjustment_factor = bias;
    return result;
}

double offsetMultiplier(std::int64_t exchange_time_micros) {
    return dynamicNewBiasMultipliers(exchange_time_micros).offset_multiplier;
}

double biasAdjustmentFactor(std::int64_t exchange_time_micros) {
    return dynamicNewBiasMultipliers(exchange_time_micros).bias_adjustment_factor;
}

bool isStrategyTime(std::int64_t exchange_time_micros) {
    const std::int64_t tod = timeOfDay(exchange_time_micros);
    return tod >= kStrategyBegin && tod < kClosingAuctionStart;
}

bool isExpansionTime(std::int64_t exchange_time_micros) {
    const std::int64_t tod = timeOfDay(exchange_time_micros);
    return tod >= kPositionLimitZeroUntil && tod < kClosingAuctionStart;
}

int strictAgreement(const std::array<double, 4>& heads) {
    bool positive = true;
    bool negative = true;
    for (std::size_t i = 0; i < heads.size(); ++i) {
        if (!std::isfinite(heads[i]) || heads[i] <= 0.0) {
            positive = false;
        }
        if (!std::isfinite(heads[i]) || heads[i] >= 0.0) {
            negative = false;
        }
    }
    if (positive) {
        return 1;
    }
    if (negative) {
        return -1;
    }
    return 0;
}

int strictAgreement(const std::vector<double>& heads) {
    if (heads.size() != 4) {
        return 0;
    }
    std::array<double, 4> fixed;
    for (std::size_t i = 0; i < fixed.size(); ++i) {
        fixed[i] = heads[i];
    }
    return strictAgreement(fixed);
}

std::int64_t allowedVolume(Side side,
                           std::int64_t requested,
                           std::int64_t current_position,
                           std::int64_t reserved_same_side,
                           int agreement,
                           std::int64_t lot_size) {
    const std::int64_t request = positiveRequested(requested);
    const std::int64_t reserved = positiveRequested(reserved_same_side);
    const std::int64_t lot = effectiveLot(lot_size);
    if (request <= 0) {
        return 0;
    }
    if (agreement == sideSign(side)) {
        return request;
    }

    const std::int64_t reduction = reductionCapacity(side, current_position);
    const long double available = static_cast<long double>(reduction) -
                                  static_cast<long double>(reserved);
    if (available <= 0.0L) {
        return 0;
    }
    const std::int64_t available_int = clampLongDoubleToInt64(available);
    return std::min(request, floorToLot(available_int, lot));
}

bool pendingOpeningNeedsCancel(Side side,
                               std::int64_t remaining,
                               std::int64_t current_position,
                               std::int64_t reserved_before,
                               int agreement,
                               std::int64_t lot_size) {
    const std::int64_t quantity = positiveRequested(remaining);
    if (quantity <= 0) {
        return false;
    }
    return allowedVolume(side,
                         quantity,
                         current_position,
                         reserved_before,
                         agreement,
                         lot_size) < quantity;
}

PricingInput::PricingInput()
    : prediction_permille(quietNaN()),
      agreement_heads(),
      bid_price(quietNaN()),
      ask_price(quietNaN()),
      last_price(quietNaN()),
      bid_volume(0),
      ask_volume(0),
      exchange_time_micros(0),
      static_position(0),
      current_position(0),
      reserved_same_side(0),
      global_buy_skew_bps(0.0),
      global_sell_skew_bps(0.0) {
    agreement_heads.fill(quietNaN());
}

PricingOutput::PricingOutput()
    : valid(false),
      agreement(0),
      signal(Signal::None),
      side(Side::Buy),
      kind(OrderKind::Hit),
      speedbag_volume(0),
      requested_volume(0),
      allowed_volume(0),
      timeout_micros(0),
      order_price(0.0),
      offset_multiplier(0.0),
      bias_adjustment_factor(0.0),
      offset(0.0),
      quote_offset(0.0),
      theo(0.0),
      bias_multiplier(0.0),
      effective_bias_factor(0.0),
      unit_bias(0.0),
      bias(0.0),
      fixed_skew(0.0),
      hit_buy_theo(0.0),
      hit_sell_theo(0.0),
      quote_buy_theo(0.0),
      quote_sell_theo(0.0),
      buy_margin(0.0),
      sell_margin(0.0),
      quote_buy_price(0.0),
      quote_sell_price(0.0) {}

std::int64_t positionLimitedVolume(Side side,
                                   std::int64_t requested,
                                   std::int64_t current_position,
                                   std::int64_t static_position,
                                   std::int64_t exchange_time_micros,
                                   double position_limit_factor,
                                   std::int64_t lot_size) {
    const std::int64_t request = positiveRequested(requested);
    const std::int64_t lot = effectiveLot(lot_size);
    if (request <= 0 || !std::isfinite(position_limit_factor) ||
        position_limit_factor < 0.0) {
        return 0;
    }
    const double nonnegative_static = static_position > 0 ?
        static_cast<double>(static_position) : 0.0;
    std::int64_t limit = floorNonnegative(nonnegative_static * position_limit_factor);
    if (!isExpansionTime(exchange_time_micros)) {
        limit = 0;
    }
    const long double capacity = side == Side::Buy
        ? static_cast<long double>(limit) - static_cast<long double>(current_position)
        : static_cast<long double>(limit) + static_cast<long double>(current_position);
    if (capacity <= 0.0L) {
        return 0;
    }
    const std::int64_t capacity_int = clampLongDoubleToInt64(capacity);
    return std::min(request, floorToLot(capacity_int, lot));
}

V06StrategyRules::V06StrategyRules(const Config& config) : config_(config) {}

const Config& V06StrategyRules::config() const {
    return config_;
}

std::int64_t V06StrategyRules::positionLimitedVolume(
    Side side,
    std::int64_t requested,
    std::int64_t current_position,
    std::int64_t static_position,
    std::int64_t exchange_time_micros) const {
    return v06_strategy::positionLimitedVolume(side,
                                                requested,
                                                current_position,
                                                static_position,
                                                exchange_time_micros,
                                                config_.position_limit_factor,
                                                config_.lot_size);
}

PricingOutput V06StrategyRules::price(const PricingInput& input) const {
    PricingOutput output;
    output.agreement = strictAgreement(input.agreement_heads);

    if (!isStrategyTime(input.exchange_time_micros) ||
        !std::isfinite(input.prediction_permille) ||
        !finitePositive(input.bid_price) || !finitePositive(input.ask_price) ||
        !std::isfinite(input.global_buy_skew_bps) ||
        !std::isfinite(input.global_sell_skew_bps) ||
        !std::isfinite(config_.offset_permille) || config_.offset_permille < 0.0 ||
        !std::isfinite(config_.quote_ratio) || config_.quote_ratio < 0.0 ||
        !std::isfinite(config_.skewness_bps) ||
        !std::isfinite(config_.bias_factor) || config_.bias_factor < 0.0 ||
        !finitePositive(config_.position_base_line) ||
        !finitePositive(config_.tick_size) || config_.lot_size <= 0 ||
        config_.position_limit_factor < 0.0 ||
        !std::isfinite(config_.position_limit_factor)) {
        return output;
    }

    const double mid = (input.bid_price + input.ask_price) * 0.5;
    if (!finitePositive(mid)) {
        return output;
    }
    const double last_price = finitePositive(input.last_price) ? input.last_price : mid;
    const DynamicNewBiasMultipliers schedule =
        dynamicNewBiasMultipliers(input.exchange_time_micros);
    const double offset = config_.offset_permille / 1000.0 * schedule.offset_multiplier;
    const double quote_offset = offset * config_.quote_ratio;
    const double theo = (1.0 + input.prediction_permille / 1000.0) * mid;
    const double bias_multiplier = last_price / config_.position_base_line;
    const double effective_bias_factor =
        config_.bias_factor * schedule.bias_adjustment_factor;
    const double unit_bias = offset * effective_bias_factor * bias_multiplier;
    const double bias = input.static_position == 0
        ? (input.current_position >= 0 ? 2.0 : -2.0)
        : std::max(-2.0,
                   std::min(2.0,
                            static_cast<double>(input.current_position) *
                                effective_bias_factor * bias_multiplier));
    const double fixed_skew = config_.skewness_bps / 10000.0;
    const double global_buy_skew = input.global_buy_skew_bps / 10000.0;
    const double global_sell_skew = input.global_sell_skew_bps / 10000.0;

    const double b_offset = 1.0 - bias * offset - offset - fixed_skew - global_buy_skew;
    const double s_offset = 1.0 - bias * offset + offset - fixed_skew + global_sell_skew;
    const double qb_offset =
        1.0 - bias * offset - quote_offset - fixed_skew - global_buy_skew;
    const double qs_offset =
        1.0 - bias * offset + quote_offset - fixed_skew + global_sell_skew;
    const double hit_buy_theo = b_offset * theo;
    const double hit_sell_theo = s_offset * theo;
    const double quote_buy_theo = qb_offset * theo;
    const double quote_sell_theo = qs_offset * theo;
    if (!std::isfinite(offset) || !std::isfinite(quote_offset) ||
        !std::isfinite(theo) || !std::isfinite(bias_multiplier) ||
        !std::isfinite(effective_bias_factor) || !std::isfinite(unit_bias) ||
        !std::isfinite(bias) || !std::isfinite(hit_buy_theo) ||
        !std::isfinite(hit_sell_theo) || !std::isfinite(quote_buy_theo) ||
        !std::isfinite(quote_sell_theo)) {
        return output;
    }

    output.valid = true;
    output.offset_multiplier = schedule.offset_multiplier;
    output.bias_adjustment_factor = schedule.bias_adjustment_factor;
    output.offset = offset;
    output.quote_offset = quote_offset;
    output.theo = theo;
    output.bias_multiplier = bias_multiplier;
    output.effective_bias_factor = effective_bias_factor;
    output.unit_bias = unit_bias;
    output.bias = bias;
    output.fixed_skew = fixed_skew;
    output.hit_buy_theo = hit_buy_theo;
    output.hit_sell_theo = hit_sell_theo;
    output.quote_buy_theo = quote_buy_theo;
    output.quote_sell_theo = quote_sell_theo;
    output.buy_margin = hit_buy_theo / input.ask_price - 1.0;
    output.sell_margin = hit_sell_theo > 0.0
        ? input.bid_price / hit_sell_theo - 1.0
        : -std::numeric_limits<double>::infinity();

    double quote_buy_price = input.bid_price < 1.0e-3
        ? input.ask_price
        : input.bid_price + config_.tick_size;
    if (quote_buy_price < config_.tick_size) {
        quote_buy_price = input.ask_price;
    }
    double quote_sell_price = input.ask_price < 1.0e-3
        ? input.bid_price
        : input.ask_price - config_.tick_size;
    if (quote_sell_price < config_.tick_size) {
        quote_sell_price = input.bid_price;
    }
    if (!finitePositive(quote_buy_price) || !finitePositive(quote_sell_price)) {
        return output;
    }
    output.quote_buy_price = quote_buy_price;
    output.quote_sell_price = quote_sell_price;

    if (input.static_position > 0 && output.buy_margin > 0.0) {
        output.signal = Signal::HitBuy;
        output.side = Side::Buy;
        output.kind = OrderKind::Hit;
        output.order_price = input.ask_price;
        output.speedbag_volume = rawHitVolume(effective_bias_factor,
                                               unit_bias,
                                               output.buy_margin,
                                               input.ask_volume,
                                               config_.lot_size);
        output.timeout_micros = config_.hit_timeout_micros;
    } else if (output.sell_margin > 0.0) {
        output.signal = Signal::HitSell;
        output.side = Side::Sell;
        output.kind = OrderKind::Hit;
        output.order_price = input.bid_price;
        output.speedbag_volume = rawHitVolume(effective_bias_factor,
                                               unit_bias,
                                               output.sell_margin,
                                               input.bid_volume,
                                               config_.lot_size);
        output.timeout_micros = config_.hit_timeout_micros;
    } else if (input.static_position > 0 && quote_buy_price <= quote_buy_theo) {
        output.signal = Signal::QuoteBuy;
        output.side = Side::Buy;
        output.kind = OrderKind::Quote;
        output.order_price = quote_buy_price;
        output.speedbag_volume = positiveRequested(input.ask_volume);
        output.timeout_micros = 0;
    } else if (quote_sell_price >= quote_sell_theo) {
        output.signal = Signal::QuoteSell;
        output.side = Side::Sell;
        output.kind = OrderKind::Quote;
        output.order_price = quote_sell_price;
        output.speedbag_volume = positiveRequested(input.bid_volume);
        output.timeout_micros = 0;
    }

    if (output.signal == Signal::None) {
        return output;
    }

    output.requested_volume = v06_strategy::positionLimitedVolume(
        output.side,
        output.speedbag_volume,
        input.current_position,
        input.static_position,
        input.exchange_time_micros,
        config_.position_limit_factor,
        config_.lot_size);
    output.allowed_volume = allowedVolume(output.side,
                                          output.requested_volume,
                                          input.current_position,
                                          input.reserved_same_side,
                                          output.agreement,
                                          config_.lot_size);
    return output;
}

PositionClampInput::PositionClampInput()
    : requested(0),
      side(Side::Buy),
      kind(OrderKind::Hit),
      current_position(0),
      static_position(0),
      opening_position(0),
      long_position(0),
      short_position(0),
      dirty_buy_hit(0),
      dirty_buy_quote(0),
      dirty_sell_hit(0),
      dirty_sell_quote(0),
      exchange_time_micros(0) {}

std::int64_t ordinaryAllowedVolume(const PositionClampInput& input,
                                   const Config& config) {
    const std::int64_t request = positiveRequested(input.requested);
    const std::int64_t lot = effectiveLot(config.lot_size);
    if (request <= 0 || input.static_position < 0 ||
        !std::isfinite(config.position_limit_factor) ||
        config.position_limit_factor < 0.0) {
        return 0;
    }

    // Zero target means liquidation only, not zero sell capacity.
    // Outstanding sells continue to consume capacity until terminal replies.
    if (input.static_position == 0) {
        if (input.side != Side::Sell) return 0;
        const long double reserved = sumNonnegative(input.dirty_sell_hit, input.dirty_sell_quote);
        const long double remaining = std::min(
            static_cast<long double>(positiveRequested(input.current_position)),
            std::max(0.0L, static_cast<long double>(positiveRequested(input.opening_position)) -
                              positiveRequested(input.short_position)));
        return floorToLot(clampLongDoubleToInt64(
            std::min(static_cast<long double>(request), std::max(0.0L, remaining - reserved))), lot);
    }

    std::int64_t limit = floorNonnegative(
        static_cast<double>(input.static_position) * config.position_limit_factor);
    if (!isExpansionTime(input.exchange_time_micros)) {
        limit = 0;
    }

    const std::int64_t dirty_buy =
        sumNonnegative(input.dirty_buy_hit, input.dirty_buy_quote);
    const std::int64_t dirty_sell =
        sumNonnegative(input.dirty_sell_hit, input.dirty_sell_quote);
    const std::int64_t dirty = input.side == Side::Buy
        ? (input.kind == OrderKind::Hit ? positiveRequested(input.dirty_buy_hit)
                                        : positiveRequested(input.dirty_buy_quote))
        : (input.kind == OrderKind::Hit ? positiveRequested(input.dirty_sell_hit)
                                        : positiveRequested(input.dirty_sell_quote));

    const long double max_opening =
        std::max<long double>(0.0L, static_cast<long double>(input.opening_position));
    const long double static_position = static_cast<long double>(input.static_position);
    const long double long_position =
        std::max<long double>(0.0L, static_cast<long double>(input.long_position));
    const long double short_position =
        std::max<long double>(0.0L, static_cast<long double>(input.short_position));
    const long double can_long_cumulative =
        std::max<long double>(0.0L, static_position - long_position);
    const long double can_short_cumulative =
        std::max<long double>(
            0.0L,
            std::min(static_position, max_opening) - short_position);

    const long double current = static_cast<long double>(input.current_position);
    const long double ordinary_capacity = input.side == Side::Buy
        ? static_cast<long double>(limit) - current -
              static_cast<long double>(dirty_buy) + static_cast<long double>(dirty_sell)
        : static_cast<long double>(limit) + current -
              static_cast<long double>(dirty_sell) + static_cast<long double>(dirty_buy);
    const long double less_num = input.side == Side::Buy
        ? std::min(std::max(static_cast<long double>(request) - dirty, 0.0L),
                   can_long_cumulative)
        : std::min(std::max(static_cast<long double>(request) - dirty, 0.0L),
                   can_short_cumulative);
    const long double capacity = std::min(std::max(ordinary_capacity, 0.0L), less_num);
    return floorToLot(clampLongDoubleToInt64(capacity), lot);
}

GlobalExposureInstrument::GlobalExposureInstrument()
    : static_position_after_adjustment(0), position_after_adjustment(0), open_price(0.0) {}

GlobalExposureInput::GlobalExposureInput() : net_traded_amount(0.0) {}

GlobalExposureResult::GlobalExposureResult()
    : valid(false), base_value(0.0), initial_net(0.0), net_traded_amount(0.0), exposure(0.0) {}

GlobalExposureResult globalExposure(const GlobalExposureInput& input) {
    GlobalExposureResult result;
    if (input.instruments.empty() ||
        !std::isfinite(input.net_traded_amount)) {
        return result;
    }

    long double base = 0.0L;
    long double initial_net = 0.0L;
    const long double traded = static_cast<long double>(input.net_traded_amount);
    for (std::size_t i = 0; i < input.instruments.size(); ++i) {
        const GlobalExposureInstrument& instrument = input.instruments[i];
        if (instrument.static_position_after_adjustment < 0 ||
            !finitePositive(instrument.open_price)) {
            return result;
        }
        base += static_cast<long double>(instrument.static_position_after_adjustment) *
                static_cast<long double>(instrument.open_price);
        initial_net += (static_cast<long double>(instrument.position_after_adjustment) -
                        static_cast<long double>(
                            instrument.static_position_after_adjustment)) *
                       static_cast<long double>(instrument.open_price);
    }

    if (!std::isfinite(static_cast<double>(base)) || base <= 0.0L ||
        !std::isfinite(static_cast<double>(initial_net)) ||
        !std::isfinite(static_cast<double>(traded))) {
        return result;
    }
    const long double exposure = (initial_net + traded) / base;
    if (!std::isfinite(static_cast<double>(exposure))) {
        return result;
    }
    result.valid = true;
    result.base_value = static_cast<double>(base);
    result.initial_net = static_cast<double>(initial_net);
    result.net_traded_amount = static_cast<double>(traded);
    result.exposure = static_cast<double>(exposure);
    return result;
}

DirectionalSkew::DirectionalSkew() : valid(false), buy_bps(0.0), sell_bps(0.0) {}

DirectionalSkew directionalSkew(double exposure,
                                double max_exposure,
                                double max_skewness_bps) {
    DirectionalSkew result;
    if (!std::isfinite(exposure) || !finitePositive(max_exposure) ||
        !std::isfinite(max_skewness_bps) || max_skewness_bps < 0.0) {
        return result;
    }
    double normalized = std::fabs(exposure) / max_exposure;
    if (normalized > 1.0) {
        normalized = 1.0;
    }
    const double penalty = normalized * normalized * max_skewness_bps;
    result.valid = std::isfinite(penalty);
    if (!result.valid) {
        return result;
    }
    if (exposure > 0.0) {
        result.buy_bps = penalty;
    } else if (exposure < 0.0) {
        result.sell_bps = penalty;
    }
    return result;
}

DirectionalSkew directionalSkew(const GlobalExposureResult& exposure,
                                const Config& config) {
    if (!exposure.valid) {
        return DirectionalSkew();
    }
    return directionalSkew(exposure.exposure,
                           config.max_exposure,
                           config.max_global_skewness_bps);
}

bool reservationTerminal(ReservationStatus status) {
    return status == ReservationStatus::Filled ||
           status == ReservationStatus::Cancelled ||
           status == ReservationStatus::TimedOut;
}

bool reservationStillHeld(ReservationStatus status) {
    return !reservationTerminal(status);
}

Reservation::Reservation()
    : order_id(0),
      side(Side::Buy),
      kind(OrderKind::Hit),
      remaining(0),
      status(ReservationStatus::Pending),
      opening(true) {}

bool ReservationBook::reserve(std::int64_t order_id,
                              Side side,
                              OrderKind kind,
                              std::int64_t volume,
                              bool opening) {
    if (volume <= 0) {
        return false;
    }
    Reservation value;
    value.order_id = order_id;
    value.side = side;
    value.kind = kind;
    value.remaining = volume;
    value.status = ReservationStatus::Pending;
    value.opening = opening;
    reservations_[order_id] = value;
    return true;
}

bool ReservationBook::requestCancel(std::int64_t order_id) {
    std::map<std::int64_t, Reservation>::iterator it = reservations_.find(order_id);
    if (it == reservations_.end() || reservationTerminal(it->second.status)) {
        return false;
    }
    it->second.status = ReservationStatus::CancelPending;
    return true;
}

bool ReservationBook::update(std::int64_t order_id,
                             ReservationStatus status,
                             std::int64_t remaining) {
    std::map<std::int64_t, Reservation>::iterator it = reservations_.find(order_id);
    if (it == reservations_.end()) {
        return false;
    }
    if (reservationTerminal(status)) {
        reservations_.erase(it);
        return true;
    }
    it->second.status = status;
    it->second.remaining = positiveRequested(remaining);
    return true;
}

bool ReservationBook::contains(std::int64_t order_id) const {
    return reservations_.find(order_id) != reservations_.end();
}

std::int64_t ReservationBook::reservedVolume(Side side) const {
    long double total = 0.0L;
    for (std::map<std::int64_t, Reservation>::const_iterator it = reservations_.begin();
         it != reservations_.end(); ++it) {
        if (it->second.side == side && reservationStillHeld(it->second.status)) {
            total += static_cast<long double>(positiveRequested(it->second.remaining));
        }
    }
    return clampLongDoubleToInt64(total);
}

std::int64_t ReservationBook::reservedVolume(Side side, OrderKind kind) const {
    long double total = 0.0L;
    for (std::map<std::int64_t, Reservation>::const_iterator it = reservations_.begin();
         it != reservations_.end(); ++it) {
        if (it->second.side == side && it->second.kind == kind &&
            reservationStillHeld(it->second.status)) {
            total += static_cast<long double>(positiveRequested(it->second.remaining));
        }
    }
    return clampLongDoubleToInt64(total);
}

std::vector<std::int64_t> ReservationBook::openingOrdersToRevoke(
    Side side,
    std::int64_t current_position,
    int agreement,
    std::int64_t lot_size) const {
    std::vector<std::int64_t> result;
    std::int64_t reserved_before = 0;
    for (std::map<std::int64_t, Reservation>::const_iterator it = reservations_.begin();
         it != reservations_.end(); ++it) {
        const Reservation& reservation = it->second;
        if (reservation.side != side || !reservationStillHeld(reservation.status) ||
            reservation.remaining <= 0) {
            continue;
        }
        // Do not emit duplicate cancel requests, but still count a pending
        // cancellation in reserved_before below until its terminal update.
        if (reservation.status != ReservationStatus::CancelPending &&
            reservation.opening &&
            pendingOpeningNeedsCancel(side,
                                      reservation.remaining,
                                      current_position,
                                      reserved_before,
                                      agreement,
                                      lot_size)) {
            result.push_back(reservation.order_id);
        }
        // A cancel request still contributes here until the terminal broker
        // update, preserving the same reservation barrier as the backtest.
        const long double next = static_cast<long double>(reserved_before) +
                                 static_cast<long double>(reservation.remaining);
        reserved_before = clampLongDoubleToInt64(next);
    }
    return result;
}

std::vector<Reservation> ReservationBook::reservations() const {
    std::vector<Reservation> result;
    result.reserve(reservations_.size());
    for (std::map<std::int64_t, Reservation>::const_iterator it = reservations_.begin();
         it != reservations_.end(); ++it) {
        result.push_back(it->second);
    }
    return result;
}

}  // namespace v06_strategy
