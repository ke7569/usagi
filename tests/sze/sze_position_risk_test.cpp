#include "common/strategy/sze_position_risk.h"

#include <cassert>
#include <cmath>

int main() {
    using namespace sze_position_risk;

    StartupPosition position = NormalizeStartupPosition(1000, 1200, 900);
    assert(position.total == 1200);
    assert(position.available == 900);
    assert(position.delta_from_static == 200);

    position = NormalizeStartupPosition(1000, -10, 500);
    assert(position.total == 0);
    assert(position.available == 0);
    assert(position.delta_from_static == -1000);

    // Baseline inventory that is unavailable cannot fund another T0 buy.
    assert(MaxCanBuy(0, 300, 0, 0) == 0);
    // A zero holding can restore the configured baseline through negative pi.
    assert(MaxCanBuy(0, 300, -1000, 0) == 1000);
    assert(MaxCanBuy(1000, 300, 0, 100) == 200);

    assert(MaxCanSell(0, 300, 1000, 0, 0) == 0);
    assert(MaxCanSell(1000, 300, 1000, 0, 100) == 200);
    assert(MaxCanSell(1000, 300, 1000, -1000, 0) == 0);

    assert(std::fabs(Bias(50000.0, 100000.0, 2.0) - 1.0) < 1e-12);
    assert(Bias(50000.0, 0.0, 2.0) == 0.0);
    assert(std::fabs(UnitBias(0.001, 2.0, 10.0, 100000.0) - 2e-7) < 1e-15);
    assert(!AtOrAfterCutoff(93059, 93100));
    assert(AtOrAfterCutoff(93100, 93100));
    assert(AtOrAfterCutoff(130000, 93100));
    assert(!StartupWarmupActive(0, 50));
    assert(StartupWarmupActive(1, 50));
    assert(StartupWarmupActive(50, 50));
    assert(!StartupWarmupActive(51, 50));
    assert(!StartupWarmupActive(1, 0));
    return 0;
}
