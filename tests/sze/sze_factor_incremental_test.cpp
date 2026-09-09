// Compile the private book in this test TU to compare its aggregates with an
// independent order-by-order oracle without exporting implementation details.
#include "sze/sampling/mix153060_runtime.cpp"
#include <cassert>
#include <iostream>

static void check(const mix153060::NativeBook& book, int64_t now) {
    const int64_t now_seconds = now / 1000000LL;
    for (int side = 0; side < 2; ++side) {
        book.for_each_level(side != 0, [now_seconds](const mix153060::NativeLevel& level) {
            __int128 insertion_sum = 0, ages = 0;
            int64_t young = 0, volume = 0;
            for (const auto& item : level.orders) {
                const auto& order = item.second;
                insertion_sum += order.insert_seconds;
                ages += now_seconds - order.insert_seconds;
                volume += order.remaining;
                if (now_seconds - order.insert_seconds <= 30) young += order.remaining;
            }
            assert(insertion_sum == level.insert_sum_seconds);
            assert(ages == static_cast<__int128>(now_seconds) * level.orders.size() - level.insert_sum_seconds);
            assert(young == level.young_volume);
            assert(volume == level.volume);
        });
    }
}

static int64_t young_buy_volume(const mix153060::NativeBook& book) {
    int64_t volume = 0;
    book.for_each_level(true, [&volume](const mix153060::NativeLevel& level) {
        volume += level.young_volume;
    });
    return volume;
}

static void test_flow_weight_interpolation() {
    double previous = 2.0;
    for (int i = 0; i <= 160000; ++i) {
        const double x = -8.0 + i * 0.0001;
        const double weight = mix153060::flow_weight(x);
        assert(std::isfinite(weight) && weight >= 0.0 && weight <= 2.0);
        assert(weight <= previous + 1e-15);
        previous = weight;
        const double exact = 1.0 - std::tanh(x);
        // Interior error is interpolation; exterior error includes the
        // intentional endpoint saturation used by the Shanghai branch.
        const double bound = std::abs(x) <= 4.0 ? 1.5e-6 : 6.71e-4;
        assert(std::abs(weight - exact) <= bound);
        assert(std::abs(weight + mix153060::flow_weight(-x) - 2.0) <= 1e-14);
    }
    assert(mix153060::flow_weight(0.0) == 1.0);
    assert(mix153060::flow_weight(100.0) == mix153060::flow_weight(4.0));
    assert(mix153060::flow_weight(-100.0) == mix153060::flow_weight(-4.0));
}

int main() {
    test_flow_weight_interpolation();
    const int64_t start = 1788921000750000LL;
    mix153060::NativeBook book;
    mix153060::OrderEvent order;
    order.kind = mix153060::OrderKind::kLimit;
    order.volume = 100;
    order.price = 10.0;
    order.buy = true;
    order.exchange_time_us = order.local_time_us = start;
    // Use a fractional second and a large cohort to check the quantized sum.
    for (int64_t id = 1; id <= 10000; ++id) {
        order.app_sequence = id;
        assert(book.add(order));
    }
    check(book, start);
    book.for_each_level(true, [start](const mix153060::NativeLevel& level) {
        assert(level.orders.begin()->second.insert_seconds == start / 1000000LL);
    });
    order.app_sequence = 10001;
    order.buy = false;
    order.price = 10.01;
    order.exchange_time_us = order.local_time_us = start + 30000000LL;
    assert(book.add(order));
    check(book, order.exchange_time_us); // Exact boundary is young.

    mix153060::TradeEvent trade;
    trade.kind = mix153060::TradeKind::kFill;
    trade.buy_order_id = 1;
    trade.sell_order_id = 10001;
    trade.volume = 40;
    trade.exchange_time_us = start + 30000000LL;
    assert(book.fill(trade));
    check(book, trade.exchange_time_us);
    trade.volume = 60;
    trade.exchange_time_us += 1;
    assert(book.fill(trade));
    check(book, trade.exchange_time_us); // Still 30 quantized seconds: young.
    assert(young_buy_volume(book) == 999900);

    order.app_sequence = 10005;
    order.exchange_time_us = order.local_time_us = (start / 1000000LL + 31) * 1000000LL - 1;
    assert(book.add(order));
    assert(young_buy_volume(book) == 999900);
    order.app_sequence = 10006;
    order.exchange_time_us = order.local_time_us = (start / 1000000LL + 31) * 1000000LL;
    assert(book.add(order));
    check(book, order.exchange_time_us);
    assert(young_buy_volume(book) == 0); // Expire at the next whole second.
    trade.exchange_time_us = order.exchange_time_us;

    // Last-order removal and same-price reuse leave stale queue entries.
    order.price = 10.02;
    order.app_sequence = 10002;
    order.exchange_time_us = order.local_time_us = trade.exchange_time_us;
    assert(book.add(order));
    trade.kind = mix153060::TradeKind::kCancel;
    trade.buy_order_id = 0;
    trade.sell_order_id = 10002;
    assert(book.fill(trade));
    order.app_sequence = 10003;
    assert(book.add(order));
    check(book, trade.exchange_time_us);
    order.app_sequence = 10004;
    order.exchange_time_us += 31000000LL;
    assert(book.add(order));
    check(book, order.exchange_time_us);
    book.clear();
    order.app_sequence = 1;
    order.exchange_time_us = start;
    assert(book.add(order));
    check(book, start); // Reset also discards expiry state.
    std::cout << "sze_factor_incremental_test: PASS\n";
}
