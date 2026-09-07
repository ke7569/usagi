#ifndef SHSZ_PREDICTOR_TRANSITION_ADAPTER_H
#define SHSZ_PREDICTOR_TRANSITION_ADAPTER_H

#include "common/contracts/legacy/RawDataStruct.h"
#include "sze/factors/shsz_full_orderbook_factor.h"

enum ShSzPredictorInputMode {
    SHSZ_PREDICTOR_INPUT_INVALID = 0,
    SHSZ_PREDICTOR_INPUT_LEGACY_SNAPSHOT = 1,
    SHSZ_PREDICTOR_INPUT_FULL_ORDERBOOK = 2
};

struct ShSzPredictorTransitionInput {
    ShSzPredictorInputMode mode;
    bool valid;
    bool has_signal_snapshot;
    MSMarketData signal_snapshot_storage;
    ShSzFullOrderBookPredictorInput full_orderbook_input;

    ShSzPredictorTransitionInput();

    const MSMarketDataField* signal_snapshot_ptr() const;
    bool uses_full_orderbook() const;
    bool uses_legacy_snapshot() const;
};

class ShSzPredictorTransitionAdapter {
public:
    static ShSzPredictorTransitionInput from_legacy_snapshot(const MSMarketDataField* snapshot);
    static ShSzPredictorTransitionInput from_full_orderbook(const ShSzFullOrderBookManager& manager,
                                                            const char* instrument_id,
                                                            const ShSzOrderFlowSummary& order_flow,
                                                            uint32_t now_time_ms,
                                                            double fee_share,
                                                            const MSMarketDataField* signal_seed);
    static MSMarketData project_signal_snapshot(const ShSzFullOrderBookEngine& engine,
                                                uint32_t now_time_ms,
                                                const MSMarketDataField* seed_snapshot);
};

#endif // SHSZ_PREDICTOR_TRANSITION_ADAPTER_H
