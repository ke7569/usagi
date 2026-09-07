#ifndef T0_STRATEGY_INS_PARAMS_H
#define T0_STRATEGY_INS_PARAMS_H

#include <cstdint>

struct InsParams {
    std::int32_t Date = 0;
    double Close = 0.0;
    double Amount = 0.0;
    double Range = 0.0;
    double HistoryAmount = 0.0;
    double FreeShare = 0.0;
    double HpUpperPrice = 0.0;
    double HpLowerPrice = 0.0;
    double HpFeeShare = 0.0;
    double HistoryVolatility20d = 0.0;
    bool HasHistoryVolatility20d = false;
    std::int32_t static_position = 0;
    std::int32_t last_position = 0;
};

#endif
