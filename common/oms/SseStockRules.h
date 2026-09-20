#ifndef USAGI_OMS_SSE_STOCK_RULES_H
#define USAGI_OMS_SSE_STOCK_RULES_H
#include "common/oms/Types.h"

namespace oms {
inline bool is_sse_star(const Instrument& instrument) {
    return instrument.market == "SSE" && instrument.code.size() == 6 &&
        (instrument.code.compare(0, 3, "688") == 0 || instrument.code.compare(0, 3, "689") == 0);
}
// Limit and simulated limit-then-cancel orders: 200..100000, in single shares.
// A sub-200 sale must liquidate the entire sellable remainder, with no other
// sell reservation. Never round a strategy request UP to reach the minimum.
inline bool star_quantity_valid(Side side, Quantity quantity, const Position& position,
                                OrderType type = OrderType::Limit) {
    const Quantity maximum = type == OrderType::NativeFak ? 50000 : 100000;
    if (quantity <= 0 || quantity > maximum) return false;
    return quantity >= 200 || (side == Side::Sell && position.working_sell == 0 &&
        quantity == position.sellable);
}
}
#endif
