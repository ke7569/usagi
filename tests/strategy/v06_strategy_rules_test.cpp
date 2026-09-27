#include "common/strategy/V06StrategyRules.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

using namespace v06_strategy;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, double tolerance, const char* message) {
    if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

std::int64_t tod(int hour, int minute, int second = 0) {
    return static_cast<std::int64_t>(hour) * 60LL * 60LL * 1000000LL +
           static_cast<std::int64_t>(minute) * 60LL * 1000000LL +
           static_cast<std::int64_t>(second) * 1000000LL;
}

void test_reference_agreement_and_volume_fixtures() {
    require(strictAgreement(std::array<double, 4>{{1.0, 2.0, 3.0, 4.0}}) == 1,
            "positive agreement");
    require(strictAgreement(std::array<double, 4>{{-1.0, -2.0, -3.0, -4.0}}) == -1,
            "negative agreement");
    require(strictAgreement(std::array<double, 4>{{1.0, 0.0, 2.0, 3.0}}) == 0,
            "zero head");
    require(strictAgreement(std::array<double, 4>{{1.0,
                                                    std::numeric_limits<double>::quiet_NaN(),
                                                    2.0,
                                                    3.0}}) == 0,
            "nonfinite head");
    require(strictAgreement(std::vector<double>{1.0, 2.0, 3.0}) == 0,
            "non-four-head input");

    require(allowedVolume(Side::Buy, 500, 100, 0, 0) == 0,
            "flat disagreement buy");
    require(allowedVolume(Side::Sell, 500, 100, 0, 0) == 100,
            "long reduction sell");
    require(allowedVolume(Side::Buy, 500, -100, 0, 0) == 100,
            "short reduction buy");
    require(allowedVolume(Side::Sell, 500, -100, 0, 0) == 0,
            "flat disagreement sell from short");
    require(allowedVolume(Side::Buy, 500, -100, 80, 0) == 0,
            "reserved short reduction");
    require(allowedVolume(Side::Sell, 500, 100, 120, 0) == 0,
            "reserved long reduction");
    require(allowedVolume(Side::Buy, 600, -250, 0, 0) == 200,
            "cross-zero disagreement is clipped");
    require(allowedVolume(Side::Buy, 600, -250, 0, 1) == 600,
            "cross-zero agreement is allowed");
    require(allowedVolume(Side::Sell, 600, -250, 0, -1) == 600,
            "same-side negative agreement");
    require(!pendingOpeningNeedsCancel(Side::Buy, 200, -250, 0, 0),
            "reduction-sized pending order remains allowed");
    require(pendingOpeningNeedsCancel(Side::Buy, 300, -250, 0, 0),
            "cross-zero pending order is revoked");
}

void test_time_schedule_and_position_barrier() {
    require(offsetMultiplier(tod(9, 30)) == 10.0, "0930 offset");
    require(offsetMultiplier(tod(9, 31)) == 5.0, "0931 offset");
    require(offsetMultiplier(tod(9, 32)) == 4.0, "0932 offset");
    require(offsetMultiplier(tod(9, 33)) == 3.0, "0933 offset");
    require(offsetMultiplier(tod(9, 34)) == 2.0, "0934 offset");
    require(offsetMultiplier(tod(9, 35)) == 1.8, "0935 offset");
    require(offsetMultiplier(tod(9, 36)) == 1.6, "0936 offset");
    require(offsetMultiplier(tod(9, 37)) == 1.4, "0937 offset");
    require(offsetMultiplier(tod(9, 38)) == 1.2, "0938 offset");
    require(offsetMultiplier(tod(9, 39)) == 1.1, "0939 offset");
    require(offsetMultiplier(tod(9, 40)) == 1.0, "0940 offset");
    require(offsetMultiplier(tod(10, 0)) == 1.0, "later offset");
    requireNear(biasAdjustmentFactor(tod(11, 59)), 1.0, 1e-12, "morning bias");
    requireNear(biasAdjustmentFactor(tod(13, 0)), 0.4 / 0.3, 1e-12, "1300 bias");
    requireNear(biasAdjustmentFactor(tod(13, 30)), 0.5 / 0.3, 1e-12, "1330 bias");
    requireNear(biasAdjustmentFactor(tod(14, 0)), 0.7 / 0.3, 1e-12, "1400 bias");
    requireNear(biasAdjustmentFactor(tod(14, 30)), 1.0 / 0.3, 1e-12, "1430 bias");

    require(!isStrategyTime(tod(9, 29, 59)), "before strategy start");
    require(isStrategyTime(tod(9, 30)), "strategy starts at 0930");
    require(isStrategyTime(tod(14, 55, 59)), "continuous close boundary");
    require(!isStrategyTime(tod(14, 56)), "closing auction boundary");
    require(!isExpansionTime(tod(9, 32, 59)), "expansion blocked before 0933");
    require(isExpansionTime(tod(9, 33)), "expansion starts at 0933");

    require(positionLimitedVolume(Side::Buy, 1000, 0, 10000, tod(9, 32)) == 0,
            "0932 buy expansion blocked");
    require(positionLimitedVolume(Side::Sell, 1000, 0, 10000, tod(9, 32)) == 0,
            "0932 sell expansion blocked");
    require(positionLimitedVolume(Side::Buy, 1000, -300, 10000, tod(9, 32)) == 300,
            "0932 short reduction remains possible");
    require(positionLimitedVolume(Side::Sell, 1000, 300, 10000, tod(9, 32)) == 300,
            "0932 long reduction remains possible");
    require(positionLimitedVolume(Side::Buy, 1000, 0, 10000, tod(9, 33)) == 1000,
            "0933 buy expansion enabled");

    PositionClampInput ordinary;
    ordinary.requested = 10000;
    ordinary.side = Side::Buy;
    ordinary.kind = OrderKind::Hit;
    ordinary.current_position = 0;
    ordinary.static_position = 10000;
    ordinary.opening_position = 10000;
    ordinary.long_position = 0;
    ordinary.short_position = 0;
    ordinary.exchange_time_micros = tod(9, 32);
    require(ordinaryAllowedVolume(ordinary) == 0,
            "full ordinary clamp keeps 0932 expansion blocked");
    ordinary.exchange_time_micros = tod(9, 33);
    require(ordinaryAllowedVolume(ordinary) == 10000,
            "full ordinary clamp enables 0933 expansion");
}

PricingInput basePricingInput() {
    PricingInput input;
    input.prediction_permille = 20.0;
    input.agreement_heads = std::array<double, 4>{{20.0, 1.0, 2.0, 3.0}};
    input.bid_price = 99.0;
    input.ask_price = 100.0;
    input.last_price = 100.0;
    input.bid_volume = 10000;
    input.ask_volume = 10000;
    input.exchange_time_micros = tod(9, 40);
    input.static_position = 10000;
    input.current_position = 0;
    input.reserved_same_side = 0;
    input.global_buy_skew_bps = 0.0;
    input.global_sell_skew_bps = 0.0;
    return input;
}

void test_pricing_and_invalid_auxiliary_head() {
    V06StrategyRules rules;
    PricingInput input = basePricingInput();
    PricingOutput priced = rules.price(input);
    require(priced.valid, "valid pricing input");
    require(priced.signal == Signal::HitBuy, "reference hit-buy pricing path");
    require(priced.agreement == 1, "positive pricing agreement");
    require(priced.speedbag_volume > 0 && priced.requested_volume > 0 &&
                priced.allowed_volume == priced.requested_volume,
            "same-side opening volume");
    requireNear(priced.offset_multiplier, 1.0, 1e-12, "pricing offset schedule");
    requireNear(priced.effective_bias_factor, 0.3, 1e-12, "pricing bias factor");
    requireNear(priced.fixed_skew, 0.0001, 1e-12, "fixed one bps skew");
    requireNear(priced.quote_offset, priced.offset * 10.0, 1e-12,
                "quote ratio");

    // An invalid auxiliary head blocks expansion but still permits a valid
    // reduction.  The main prediction remains finite and priceable.
    input.agreement_heads[1] = std::numeric_limits<double>::quiet_NaN();
    input.current_position = -300;
    priced = rules.price(input);
    require(priced.valid && priced.agreement == 0, "invalid auxiliary head gate");
    require(priced.signal == Signal::HitBuy, "invalid auxiliary head still prices");
    require(priced.allowed_volume == 300, "invalid auxiliary head reduction only");

    input.agreement_heads[1] = 0.0;
    priced = rules.price(input);
    require(priced.valid && priced.agreement == 0 && priced.allowed_volume == 300,
            "zero auxiliary head gate");

    input.prediction_permille = std::numeric_limits<double>::quiet_NaN();
    priced = rules.price(input);
    require(!priced.valid && priced.allowed_volume == 0,
            "nonfinite main prediction is not priceable");
}

void test_pending_cancellation_keeps_reservation() {
    ReservationBook book;
    require(book.reserve(10, Side::Buy, OrderKind::Quote, 200, true),
            "reserve first order");
    require(book.reserve(11, Side::Buy, OrderKind::Hit, 100, true),
            "reserve second order");
    require(book.reservedVolume(Side::Buy) == 300, "initial reservation");
    require(book.requestCancel(10), "cancel request");
    require(book.reservedVolume(Side::Buy) == 300,
            "cancel pending reservation is retained");

    const std::vector<std::int64_t> revoke =
        book.openingOrdersToRevoke(Side::Buy, -250, 0);
    require(revoke.size() == 1 && revoke[0] == 11,
            "later opening order revoked after reserved reduction");
    require(book.update(10, ReservationStatus::Cancelled, 0),
            "cancel confirmation");
    require(book.reservedVolume(Side::Buy) == 100,
            "terminal cancel releases reservation");
    require(book.contains(11), "nonterminal order retained");
    require(book.update(11, ReservationStatus::PartialFill, 40),
            "partial fill update");
    require(book.reservedVolume(Side::Buy) == 40, "partial remaining reservation");
    require(book.update(11, ReservationStatus::Filled, 0), "fill confirmation");
    require(book.reservedVolume(Side::Buy) == 0, "filled reservation released");
}

void test_global_exposure_and_saturated_directional_penalty() {
    GlobalExposureInput input;
    GlobalExposureInstrument first;
    first.static_position_after_adjustment = 1000;
    first.position_after_adjustment = 1050;
    first.open_price = 100.0;
    GlobalExposureInstrument second;
    second.static_position_after_adjustment = 500;
    second.position_after_adjustment = 450;
    second.open_price = 200.0;
    input.instruments.push_back(first);
    input.instruments.push_back(second);
    const GlobalExposureResult zero_traded = globalExposure(input);
    require(zero_traded.valid, "multiple instruments valid before first trade");
    requireNear(zero_traded.exposure, -0.025, 1e-12, "zero trades preserve initial exposure");
    input.net_traded_amount = 15000.0;

    const GlobalExposureResult exposure = globalExposure(input);
    require(exposure.valid, "global exposure valid");
    requireNear(exposure.base_value, 200000.0, 1e-9, "global static base value");
    requireNear(exposure.initial_net, -5000.0, 1e-9, "global initial net");
    requireNear(exposure.exposure, 0.05, 1e-12, "global exposure snapshot");
    DirectionalSkew skew = directionalSkew(exposure);
    require(skew.valid, "global skew valid");
    requireNear(skew.buy_bps, 10.0, 1e-12, "positive exposure buy penalty");
    requireNear(skew.sell_bps, 0.0, 1e-12, "positive exposure no sell penalty");

    input.net_traded_amount = 100000.0;
    const GlobalExposureResult saturated = globalExposure(input);
    skew = directionalSkew(saturated);
    requireNear(saturated.exposure, 0.475, 1e-12, "exposure is not hard capped");
    requireNear(skew.buy_bps, 10.0, 1e-12, "positive penalty saturates only");
    requireNear(skew.sell_bps, 0.0, 1e-12, "saturated positive no sell penalty");

    input.net_traded_amount = -100000.0;
    skew = directionalSkew(globalExposure(input));
    require(skew.sell_bps > 0.0 && skew.buy_bps == 0.0,
            "negative exposure only sell penalty");
    input.net_traded_amount = std::numeric_limits<double>::quiet_NaN();
    require(!globalExposure(input).valid, "invalid account amount still rejected");
}

}  // namespace

int main() {
    try {
        test_reference_agreement_and_volume_fixtures();
        test_time_schedule_and_position_barrier();
        test_pricing_and_invalid_auxiliary_head();
        test_pending_cancellation_keeps_reservation();
        test_global_exposure_and_saturated_directional_penalty();
        std::cout << "v06_strategy_rules_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "v06_strategy_rules_test: " << error.what() << '\n';
        return 1;
    }
}
