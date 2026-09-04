// SSE trading catch-up gate unit test.
#include "sse_trading_gate.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using namespace sse_trading;

int main() {
    // MarketTime (HHMMSSmmm) -> micros since midnight.
    assert(market_time_to_micros(93100000.0) == 34260000000ULL);   // 09:31:00.000
    assert(market_time_to_micros(93101500.0) == 34261500000ULL);   // 09:31:01.500
    assert(market_time_to_micros(0.0) == 0ULL);

    // Gate lag.
    assert(gate_lag_us(93100000.0, 34260000000ULL) == 0);
    assert(gate_lag_us(93100000.0, 34261000000ULL) == 1000000LL); // +1s
    assert(gate_lag_us(93100000.0, 34259000000ULL) == 1000000LL); // -1s

    TradingGate gate;
    gate.configure(true, 1000000ULL);
    std::string transition;

    // No lag: allowed.
    assert(gate.permit(93100000.0, 34260000000ULL, &transition));
    assert(transition.empty() && !gate.blocked());

    // 2s lag: blocked, transition reported once.
    assert(!gate.permit(93100000.0, 34262000000ULL, &transition));
    assert(transition == "blocked" && gate.blocked());
    transition.clear();
    assert(!gate.permit(93100000.0, 34262000000ULL, &transition));
    assert(transition.empty() && gate.blocked());

    // Back within 1s: resumed, transition reported once.
    assert(gate.permit(93100000.0, 34260500000ULL, &transition));
    assert(transition == "resumed" && !gate.blocked());
    transition.clear();
    assert(gate.permit(93100000.0, 34260500000ULL, &transition));
    assert(transition.empty() && !gate.blocked());

    // Disabled gate always permits.
    gate.configure(false, 1000000ULL);
    assert(gate.permit(93100000.0, 34263000000ULL, &transition));
    assert(!gate.blocked());

    // Global gate is a single process-wide instance.
    TradingGate& global = global_trading_gate();
    (void)&global;

    std::cout << "sse_trading_gate_test ok\n";
    return 0;
}
