#ifndef SZE_POSITION_RISK_H
#define SZE_POSITION_RISK_H

#include <algorithm>
#include <cstdint>

namespace sze_position_risk {

struct StartupPosition {
    std::int32_t total = 0;
    std::int32_t available = 0;
    std::int32_t delta_from_static = 0;
};

inline StartupPosition NormalizeStartupPosition(std::int32_t static_position,
                                                std::int32_t total_position,
                                                std::int32_t available_position) {
    StartupPosition result;
    result.total = std::max<std::int32_t>(total_position, 0);
    result.available = std::max<std::int32_t>(
        0, std::min<std::int32_t>(available_position, result.total));
    result.delta_from_static = result.total - static_position;
    return result;
}

inline std::int32_t MaxCanBuy(std::int32_t shortable,
                              std::int32_t position_limit,
                              std::int32_t position_delta,
                              std::int32_t pending_buy) {
    const std::int32_t qty = std::min(shortable, position_limit) -
                             position_delta - pending_buy;
    return std::max<std::int32_t>(qty, 0);
}

inline std::int32_t MaxCanSell(std::int32_t shortable,
                               std::int32_t position_limit,
                               std::int32_t static_position,
                               std::int32_t position_delta,
                               std::int32_t pending_sell) {
    const std::int32_t shortable_cap =
        std::min(shortable, position_limit + position_delta) - pending_sell;
    const std::int32_t available_cap =
        static_position + position_delta - pending_sell;
    return std::max<std::int32_t>(std::min(shortable_cap, available_cap), 0);
}

inline double Bias(double current_position_notional,
                   double position_base_line,
                   double bias_factor) {
    if (position_base_line <= 0.0) {
        return 0.0;
    }
    return current_position_notional / position_base_line * bias_factor;
}

inline double UnitBias(double offset,
                       double bias_factor,
                       double last_price,
                       double position_base_line) {
    if (position_base_line <= 0.0) {
        return 0.0;
    }
    return offset * bias_factor * last_price / position_base_line;
}

inline bool AtOrAfterCutoff(int market_hhmmss, int cutoff_hhmmss) {
    return market_hhmmss >= cutoff_hhmmss;
}

inline bool StartupWarmupActive(int sample_count, int warmup_count) {
    return sample_count > 0 && sample_count <= std::max(0, warmup_count);
}

}  // namespace sze_position_risk

#endif
