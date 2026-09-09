// Compile the private book in this test TU to compare its aggregates with an
// independent order-by-order oracle without exporting implementation details.
#include "sze/sampling/mix153060_runtime.cpp"
#include <cassert>
#include <iostream>

static void check(const mix153060::NativeBook& book, int64_t now) {
    for (int side = 0; side < 2; ++side) {
        book.for_each_level(side != 0, [now](const mix153060::NativeLevel& level) {
            __int128 insertion_sum = 0, ages = 0;
            int64_t young = 0, volume = 0;
            for (const auto& item : level.orders) {
                const auto& order = item.second;
                insertion_sum += order.insert_us;
                ages += now - order.insert_us;
                volume += order.remaining;
                if (now - order.insert_us <= 30000000LL) young += order.remaining;
            }
            assert(insertion_sum == level.insert_sum_us);
            assert(ages == static_cast<__int128>(now) * level.orders.size() - level.insert_sum_us);
            assert(young == level.young_volume);
            assert(volume == level.volume);
        });
    }
}

int main() {
    const int64_t start = 1788921000000000LL;
    mix153060::NativeBook book;
    mix153060::OrderEvent order;
    order.kind = mix153060::OrderKind::kLimit;
    order.volume = 100;
    order.price = 10.0;
    order.buy = true;
    order.exchange_time_us = order.local_time_us = start;
    // The timestamp sum exceeds int64 even though the actual age sum does not.
    for (int64_t id = 1; id <= 10000; ++id) {
        order.app_sequence = id;
        assert(book.add(order));
    }
    check(book, start);
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
    check(book, trade.exchange_time_us); // 30 seconds + 1 microsecond expires.

    // Last-order removal and same-price reuse leave stale queue entries.
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
    order.exchange_time_us += 30000001LL;
    assert(book.add(order));
    check(book, order.exchange_time_us);
    book.clear();
    order.app_sequence = 1;
    order.exchange_time_us = start;
    assert(book.add(order));
    check(book, start); // Reset also discards expiry state.
    std::cout << "sze_factor_incremental_test: PASS\n";
}
