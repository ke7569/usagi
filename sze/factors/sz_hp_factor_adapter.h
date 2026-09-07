#ifndef SZ_HP_FACTOR_ADAPTER_H
#define SZ_HP_FACTOR_ADAPTER_H

#include <array>
#include <cstddef>

#include "sze/factors/shsz_full_orderbook_factor.h"
#include "sze/market_data/sz_hp_realtime_state.h"

namespace sz_hp {

static const size_t kHpOrderTradeFactorCount = 7;
static const size_t kHpFullOrderBookFactorCount = 22;
static const size_t kHpMarketFactorCount = 21;
static const size_t kHpCobFactorCount =
    kHpOrderTradeFactorCount + kHpMarketFactorCount + kHpFullOrderBookFactorCount;
static const size_t kHpOrderTradeFullBookFactorCount =
    kHpOrderTradeFactorCount + kHpFullOrderBookFactorCount;

struct HpOrderTradeFullBookVector {
    std::array<float, kHpOrderTradeFullBookFactorCount> values;
    bool valid;

    HpOrderTradeFullBookVector();
};

struct HpCobFactorVector {
    std::array<float, kHpCobFactorCount> values;
    // FullBook::Normalized consumes the same values in a different order:
    // market factors, order/trade factors, then full-book factors.
    std::array<float, kHpCobFactorCount> model_values;
    std::array<float, kHpMarketFactorCount> market_values;
    bool valid;

    HpCobFactorVector();
};

struct FactorInput {
    ShSzOrderFlowSummary raw_order_flow;
    ShSzFullOrderBookPredictorInput full_orderbook;
    HpOrderTradeFullBookVector ordered_values;
    HpCobFactorVector cob_values;
    uint32_t event_time_ms;
    bool valid;

    FactorInput();
};

ShSzOrderFlowSummary to_shsz_order_flow(const OrderFlowSample& sample);
FactorInput build_factor_input(const InstrumentState& state,
                               const SampleBatch& batch);

// Names and order are part of the HP model contract. `hp_cob_factor_names`
// follows the HpFactor in-memory layout; `hp_cob_model_input_names` follows
// FullBook::Normalized before mean/std scaling.
const std::array<const char*, kHpCobFactorCount>& hp_cob_factor_names();
const std::array<const char*, kHpCobFactorCount>& hp_cob_model_input_names();

bool normalize_cob_model_input(const HpCobFactorVector& factors,
                               const std::array<float, kHpCobFactorCount>& mean,
                               const std::array<float, kHpCobFactorCount>& std,
                               std::array<float, kHpCobFactorCount>* output);

}  // namespace sz_hp

#endif  // SZ_HP_FACTOR_ADAPTER_H
