#ifndef T0_PREDICTOR_MIX153060_LIVE_ADAPTER_H
#define T0_PREDICTOR_MIX153060_LIVE_ADAPTER_H

#include <cstdint>
#include <string>

#include "common/contracts/legacy/LFDataStruct.h"
#include "sze/sampling/mix153060_runtime.h"

namespace mix153060 {

bool normalize_order_event(const LFL2OrderField& source,
                           int32_t trading_date,
                           int64_t receive_time,
                           OrderEvent* destination,
                           std::string* error = 0);

bool normalize_trade_event(const LFL2TradeField& source,
                           int32_t trading_date,
                           int64_t receive_time,
                           TradeEvent* destination,
                           std::string* error = 0);

}  // namespace mix153060

#endif  // T0_PREDICTOR_MIX153060_LIVE_ADAPTER_H
