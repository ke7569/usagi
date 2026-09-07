#include "sze/runtime/mix153060_live_adapter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace {

void set_text(char* destination, std::size_t capacity, const char* value) {
    std::memset(destination, 0, capacity);
    const std::size_t length = std::min(capacity - 1, std::strlen(value));
    std::memcpy(destination, value, length);
}

LFL2OrderField make_order() {
    LFL2OrderField value;
    std::memset(&value, 0, sizeof(value));
    set_text(value.OrderTime, sizeof(value.OrderTime), "09:31:02.123456");
    value.Price = 10.25;
    value.Volume = 100.0;
    value.OrderKind[0] = 'B';
    value.OrdType[0] = '2';
    value.ApplSeqNum = 101;
    return value;
}

LFL2TradeField make_trade() {
    LFL2TradeField value;
    std::memset(&value, 0, sizeof(value));
    set_text(value.TradeTime, sizeof(value.TradeTime), "09:31:02.123456");
    value.Price = 10.25;
    value.Volume = 20.0;
    value.OrderKind[0] = 'F';
    value.BidApplSeqNum = 101;
    value.OfferApplSeqNum = 202;
    value.ApplSeqNum = 303;
    return value;
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
    }
    return condition;
}

mix153060::OrderEvent runtime_order(int64_t sequence,
                                    int64_t time_us,
                                    double price,
                                    int64_t volume,
                                    bool buy,
                                    mix153060::OrderKind kind) {
    mix153060::OrderEvent value;
    value.app_sequence = sequence;
    value.exchange_time_us = time_us;
    value.local_time_us = time_us;
    value.price = price;
    value.volume = volume;
    value.buy = buy;
    value.kind = kind;
    return value;
}

mix153060::TradeEvent runtime_fill(int64_t sequence,
                                   int64_t time_us,
                                   double price,
                                   int64_t volume,
                                   int64_t buy_id,
                                   int64_t sell_id) {
    mix153060::TradeEvent value;
    value.app_sequence = sequence;
    value.exchange_time_us = time_us;
    value.local_time_us = time_us;
    value.price = price;
    value.volume = volume;
    value.buy_order_id = buy_id;
    value.sell_order_id = sell_id;
    value.kind = mix153060::TradeKind::kFill;
    return value;
}

bool test_deferred_market_order_price() {
    mix153060::StaticInputs inputs;
    inputs.instrument = "000001";
    inputs.trading_date = 20260715;
    inputs.average_amount = 1000000.0;
    inputs.turnover_threshold = 1.0;
    inputs.free_share = 100000.0;
    inputs.pre_close = 10.0;
    inputs.upper_limit = 11.0;
    inputs.lower_limit = 9.0;
    inputs.history_volatility_20d = 0.02;
    mix153060::Runtime runtime(inputs);
    mix153060::SampleBuffer samples;
    const int64_t time = 34261000000LL;

    runtime.on_order(runtime_order(1, time, 10.00, 500, true,
                                   mix153060::OrderKind::kLimit), &samples);
    runtime.on_order(runtime_order(2, time + 1000, 10.10, 100, false,
                                   mix153060::OrderKind::kLimit), &samples);
    runtime.on_order(runtime_order(3, time + 2000, 10.20, 100, false,
                                   mix153060::OrderKind::kLimit), &samples);
    runtime.on_order(runtime_order(4, time + 2500, 10.21, 200, false,
                                   mix153060::OrderKind::kLimit), &samples);
    runtime.on_order(runtime_order(5, time + 3000, 0.0, 300, true,
                                   mix153060::OrderKind::kMarket), &samples);
    if (!expect(runtime.available() && samples.count == 0,
                "market order waits when opposite best level is too small")) {
        return false;
    }

    runtime.on_trade(runtime_fill(6, time + 3000, 10.20, 100, 5, 3), &samples);
    runtime.on_trade(runtime_fill(7, time + 4000, 10.21, 200, 5, 4), &samples);
    mix153060::OrderEvent resolved;
    bool from_linked_fill = false;
    if (!expect(runtime.take_resolved_market_order(&resolved, &from_linked_fill) &&
                    std::fabs(resolved.price - 10.21) < 1.0e-9 && from_linked_fill,
                "resolved market audit carries the final linked execution price") ||
        !expect(!runtime.take_resolved_market_order(&resolved, &from_linked_fill),
                "resolved market audit is consumable exactly once")) {
        return false;
    }
    if (!expect(runtime.available() && samples.count == 0,
                "linked market fill is held until the minimatch frame closes")) {
        return false;
    }

    runtime.on_order(runtime_order(8, time + 5000, 10.30, 100, false,
                                   mix153060::OrderKind::kLimit), &samples);
    if (!expect(runtime.available() && samples.count == 1,
                "resolved market order produces its completed frame") ||
        !expect(std::fabs(samples.values[0].last_price - 10.21) < 1.0e-9,
                "market order uses final linked execution price") ||
        !expect(std::fabs(samples.values[0].factors[45] -
                          static_cast<float>(std::asinh(300.0 / 100000.0))) < 1.0e-6,
                "market flow uses the v0.4 compressed flow factor")) {
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const int32_t trading_date = 20260715;
    mix153060::StaticInputs static_inputs;
    static_inputs.instrument = "000001";
    static_inputs.trading_date = trading_date;
    static_inputs.average_amount = 800000000.0;
    static_inputs.turnover_threshold = 100000.0;
    static_inputs.pre_close = 10.25;
    static_inputs.upper_limit = 11.28;
    static_inputs.lower_limit = 9.23;
    static_inputs.history_volatility_20d = 0.018;
    mix153060::Runtime configured_runtime(static_inputs);
    if (!expect(configured_runtime.configured(), "complete static inputs") ||
        !expect(configured_runtime.available(), "configured runtime availability")) {
        return 1;
    }
    configured_runtime.invalidate();
    if (!expect(!configured_runtime.available(), "explicit runtime suppression")) {
        return 1;
    }
    static_inputs.history_volatility_20d =
        std::numeric_limits<double>::quiet_NaN();
    mix153060::Runtime missing_history_runtime(static_inputs);
    if (!expect(missing_history_runtime.configured(),
                "v0.4 does not require history volatility")) {
        return 1;
    }

    int64_t exchange_time_us = 0;
    int64_t millisecond_time_us = 0;
    if (!expect(mix153060::parse_exchange_time_us(
                    "09:31:02.123456", trading_date, &exchange_time_us),
                "microsecond exchange clock") ||
        !expect(mix153060::parse_exchange_time_us(
                    "09:31:02.123", trading_date, &millisecond_time_us),
                "millisecond exchange clock") ||
        !expect(exchange_time_us - millisecond_time_us == 456,
                "fractional clock scaling") ||
        !expect(!mix153060::parse_exchange_time_us(
                    "09:61:02.123", trading_date, &millisecond_time_us),
                "invalid exchange clock rejection") ||
        !expect(!mix153060::parse_exchange_time_us(
                    "09:31:02.123", 20260230, &millisecond_time_us),
                "invalid trading date rejection")) {
        return 1;
    }

    static_inputs.history_volatility_20d = 0.018;
    mix153060::OrderEvent native_order;
    native_order.app_sequence = 101;
    native_order.exchange_time_us = exchange_time_us;
    native_order.local_time_us = exchange_time_us;
    native_order.price = 10.25;
    native_order.volume = 100;
    native_order.buy = true;
    native_order.kind = mix153060::OrderKind::kLimit;
    mix153060::SampleBuffer native_samples;
    mix153060::Runtime duplicate_order_runtime(static_inputs);
    duplicate_order_runtime.on_order(native_order, &native_samples);
    native_order.exchange_time_us += 1000;
    native_order.local_time_us += 1000;
    duplicate_order_runtime.on_order(native_order, &native_samples);
    if (!expect(!duplicate_order_runtime.available() && native_samples.count == 0 &&
                    duplicate_order_runtime.failure_sequence() == 101 &&
                    !duplicate_order_runtime.failure_reason().empty(),
                "duplicate order fail-closed")) {
        return 1;
    }

    native_order.exchange_time_us = exchange_time_us;
    native_order.local_time_us = exchange_time_us;
    mix153060::Runtime missing_cancel_runtime(static_inputs);
    missing_cancel_runtime.on_order(native_order, &native_samples);
    mix153060::TradeEvent missing_cancel;
    missing_cancel.app_sequence = 102;
    missing_cancel.exchange_time_us = exchange_time_us + 1000;
    missing_cancel.local_time_us = exchange_time_us + 1000;
    missing_cancel.price = 0.0;
    missing_cancel.volume = 100;
    missing_cancel.buy_order_id = 999;
    missing_cancel.sell_order_id = 0;
    missing_cancel.kind = mix153060::TradeKind::kCancel;
    missing_cancel_runtime.on_trade(missing_cancel, &native_samples);
    if (!expect(!missing_cancel_runtime.available() && native_samples.count == 0 &&
                    missing_cancel_runtime.failure_sequence() == 102,
                "missing cancel fail-closed")) {
        return 1;
    }

    const int64_t backtest_receive_time = 93102123456789LL;
    if (!expect(mix153060::normalize_receive_time_us(
                    backtest_receive_time, trading_date, exchange_time_us) == exchange_time_us,
                "backtest receive timestamp") ||
        !expect(mix153060::normalize_receive_time_us(
                    exchange_time_us + 777, trading_date, exchange_time_us) ==
                    exchange_time_us + 777,
                "epoch microsecond receive timestamp") ||
        !expect(mix153060::normalize_receive_time_us(
                    (exchange_time_us + 777) * 1000LL,
                    trading_date, exchange_time_us) == exchange_time_us + 777,
                "epoch nanosecond receive timestamp") ||
        !expect(mix153060::normalize_receive_time_us(
                    0, trading_date, exchange_time_us) == exchange_time_us,
                "missing receive timestamp fallback")) {
        return 1;
    }

    const std::uint64_t reference_mono_ns = 700000000000000ULL;
    const std::uint64_t receive_mono_ns = reference_mono_ns - 25000ULL;
    // The research convention stores China wall-clock against UTC midnight.
    // A 01:31 UTC realtime instant is therefore 09:31 in the local epoch.
    const std::uint64_t reference_realtime_ns = static_cast<std::uint64_t>(
        (exchange_time_us - 8LL * 3600LL * 1000000LL) * 1000LL + 25000ULL);
    if (!expect(mix153060::recover_monotonic_receive_time_us(
                    receive_mono_ns, reference_mono_ns, reference_realtime_ns,
                    trading_date, exchange_time_us) == exchange_time_us,
                "monotonic recovery timestamp conversion") ||
        !expect(mix153060::recover_monotonic_receive_time_us(
                    0, reference_mono_ns, reference_realtime_ns,
                    trading_date, exchange_time_us) == exchange_time_us,
                "monotonic recovery timestamp fallback")) {
        return 1;
    }

    std::string error;
    mix153060::OrderEvent order_event;
    LFL2OrderField order = make_order();
    if (!expect(mix153060::normalize_order_event(
                    order, trading_date, backtest_receive_time, &order_event, &error),
                "limit order accepted") ||
        !expect(order_event.buy && order_event.kind == mix153060::OrderKind::kLimit &&
                    order_event.app_sequence == 101 && order_event.volume == 100 &&
                    order_event.exchange_time_us == exchange_time_us &&
                    order_event.local_time_us == exchange_time_us,
                "limit order normalization")) {
        return 1;
    }
    order.OrderKind[0] = 'S';
    order.OrdType[0] = '1';
    order.Price = 0.0;
    if (!expect(mix153060::normalize_order_event(
                    order, trading_date, backtest_receive_time, &order_event, &error) &&
                    !order_event.buy && order_event.kind == mix153060::OrderKind::kMarket,
                "zero-price market sell order")) {
        return 1;
    }
    order.OrdType[0] = 'U';
    if (!expect(mix153060::normalize_order_event(
                    order, trading_date, backtest_receive_time, &order_event, &error) &&
                    order_event.kind == mix153060::OrderKind::kSelfBest,
                "self-best order")) {
        return 1;
    }
    order = make_order();
    order.OrderKind[0] = 'X';
    if (!expect(!mix153060::normalize_order_event(
                    order, trading_date, backtest_receive_time, &order_event, &error),
                "unknown order side rejection")) {
        return 1;
    }
    order = make_order();
    order.OrdType[0] = 'X';
    if (!expect(!mix153060::normalize_order_event(
                    order, trading_date, backtest_receive_time, &order_event, &error),
                "unknown order type rejection")) {
        return 1;
    }
    order = make_order();
    order.Volume = 0.5;
    if (!expect(!mix153060::normalize_order_event(
                    order, trading_date, backtest_receive_time, &order_event, &error),
                "fractional order volume rejection")) {
        return 1;
    }
    order = make_order();
    order.Price = -1.0;
    if (!expect(!mix153060::normalize_order_event(
                    order, trading_date, backtest_receive_time, &order_event, &error),
                "negative order price rejection")) {
        return 1;
    }
    order = make_order();
    order.Price = std::numeric_limits<double>::max();
    if (!expect(!mix153060::normalize_order_event(
                    order, trading_date, backtest_receive_time, &order_event, &error),
                "out-of-range order price rejection")) {
        return 1;
    }
    order = make_order();
    order.Volume = std::numeric_limits<double>::infinity();
    if (!expect(!mix153060::normalize_order_event(
                    order, trading_date, backtest_receive_time, &order_event, &error),
                "non-finite order volume rejection")) {
        return 1;
    }

    mix153060::TradeEvent trade_event;
    LFL2TradeField trade = make_trade();
    if (!expect(mix153060::normalize_trade_event(
                    trade, trading_date, backtest_receive_time, &trade_event, &error),
                "fill accepted") ||
        !expect(trade_event.kind == mix153060::TradeKind::kFill &&
                    trade_event.buy_order_id == 101 && trade_event.sell_order_id == 202 &&
                    trade_event.volume == 20,
                "fill normalization")) {
        return 1;
    }
    trade.OrderKind[0] = '4';
    trade.Price = 0.0;
    trade.OfferApplSeqNum = 0;
    if (!expect(mix153060::normalize_trade_event(
                    trade, trading_date, backtest_receive_time, &trade_event, &error) &&
                    trade_event.kind == mix153060::TradeKind::kCancel,
                "zero-price cancel accepted")) {
        return 1;
    }
    trade = make_trade();
    trade.OrderKind[0] = 'X';
    if (!expect(!mix153060::normalize_trade_event(
                    trade, trading_date, backtest_receive_time, &trade_event, &error),
                "unknown trade type rejection")) {
        return 1;
    }
    trade = make_trade();
    trade.OfferApplSeqNum = 0;
    if (!expect(!mix153060::normalize_trade_event(
                    trade, trading_date, backtest_receive_time, &trade_event, &error),
                "fill missing order id rejection")) {
        return 1;
    }
    trade = make_trade();
    trade.OfferApplSeqNum = trade.BidApplSeqNum;
    if (!expect(!mix153060::normalize_trade_event(
                    trade, trading_date, backtest_receive_time, &trade_event, &error),
                "fill with duplicate order ids rejection")) {
        return 1;
    }
    trade = make_trade();
    trade.OrderKind[0] = '4';
    trade.BidApplSeqNum = 0;
    trade.OfferApplSeqNum = 0;
    if (!expect(!mix153060::normalize_trade_event(
                    trade, trading_date, backtest_receive_time, &trade_event, &error),
                "cancel missing order id rejection")) {
        return 1;
    }
    trade = make_trade();
    trade.BidApplSeqNum = -1;
    if (!expect(!mix153060::normalize_trade_event(
                    trade, trading_date, backtest_receive_time, &trade_event, &error),
                "negative trade order id rejection")) {
        return 1;
    }

    if (!test_deferred_market_order_price()) {
        return 1;
    }

    std::cout << "mix153060 live adapter tests passed\n";
    return 0;
}
