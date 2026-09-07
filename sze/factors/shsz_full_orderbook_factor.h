#ifndef SHSZ_FULL_ORDERBOOK_FACTOR_H
#define SHSZ_FULL_ORDERBOOK_FACTOR_H

#include <cstdint>

#include "common/market_data/legacy/shsz_full_orderbook_engine.h"
#include "common/market_data/legacy/shsz_full_orderbook_manager.h"
#include "sze/market_data/sz_hp_orderbook.h"

struct ShSzOrderFlowSummary {
    double order_pf;
    double order_nf;
    double order_mf;
    double trade_pcf;
    double trade_ncf;
    double trade_pt;
    double trade_nt;
    double cxl_buy_flow;
    double cxl_sell_flow;
    double buy_order_volume;
    double sell_order_volume;

    ShSzOrderFlowSummary();
};

struct ShSzFullOrderBookFactorSet {
    float PositiveFillRate;
    float NegativeFillRate;
    float OrderFlowImbalance;
    float CFRImbalance;
    float FixDisImbalancePct1;
    float FixDisImbalancePct2;
    float WeightedFixDisImbalancePct1;
    float WeightedFixDisImbalancePct2;
    float AvgSizeImbalance;
    float AvgSizeImbalanceLevel1;
    float AvgSizeImbalanceLevel5;
    float OrderCountImbalance;
    float OrderCountImbalanceLevel1;
    float OrderCountImbalanceLevel5;
    float OrderLifeImbalance;
    float OrderLifeImbalanceLevel1;
    float OrderLifeImbalanceLevel5;
    float MaxBidDistance;
    float MaxAskDistance;
    float MaxVolDistanceImbalance;
    float YoungOrderbookImbalance;
    float FixDistHermes;
    bool valid;

    ShSzFullOrderBookFactorSet();
};

struct ShSzFullOrderBookPredictorInput {
    ShSzOrderFlowSummary order_flow;
    ShSzFullOb aggregate;
    ShSzFullOrderBookFactorSet factors;
    uint32_t now_time_ms;
    double fee_share;
    bool valid;

    ShSzFullOrderBookPredictorInput();
};

class ShSzFullOrderBookFactorExtractor {
public:
    static ShSzFullOrderBookFactorSet extract(const ShSzFullOrderBookEngine& engine,
                                              const ShSzOrderFlowSummary& order_flow,
                                              uint32_t now_time_ms,
                                              double fee_share);
    static ShSzFullOrderBookFactorSet extract(const sz_hp::OrderBook& book,
                                              const ShSzOrderFlowSummary& order_flow,
                                              uint32_t now_time_ms,
                                              double fee_share);
    static ShSzFullOrderBookPredictorInput build_predictor_input(const ShSzFullOrderBookEngine& engine,
                                                                 const ShSzOrderFlowSummary& order_flow,
                                                                 uint32_t now_time_ms,
                                                                 double fee_share);
    static ShSzFullOrderBookPredictorInput build_predictor_input(const sz_hp::OrderBook& book,
                                                                 const ShSzOrderFlowSummary& order_flow,
                                                                 uint32_t now_time_ms,
                                                                 double fee_share);
    static ShSzFullOrderBookPredictorInput build_predictor_input(const ShSzFullOrderBookManager& manager,
                                                                 const char* instrument_id,
                                                                 const ShSzOrderFlowSummary& order_flow,
                                                                 uint32_t now_time_ms,
                                                                 double fee_share);
};

#endif // SHSZ_FULL_ORDERBOOK_FACTOR_H
