//
// Created by Administrator on 25-9-11.
//

#ifndef COMMON_H
#define COMMON_H


#include <string>

#include "common/factors/legacy/factor.h"

struct GlobalParams {
    double offset_base_line = 0;
    double offset = 0;
    double global_bias_factor = 0;
    double position_limit = 0;
    double position_limit_base_line = 0;
    double position_penalty_factor = 0;
    double position_base_line = 0;
    double global_pred_adj_factor = 0;
    int can_quote = 0;
};

struct InstrumentParams {
    double offset;
    double history_amount;
    double max_order_size = 1e5;
    double min_order_size = 1e3;
    int32_t static_position;
    int32_t last_position;
    std::int32_t shortable = 0;
    std::int32_t vol_unit = 100;


};


struct Theo {
    double theo0 = 0;
    double bias_factor = 0;
    double bias = 0;
    double skew = 0;
    double unitbias = 0;
    double position_base_line = 100000;
    double hit_buy_theo = 0;
    double hit_sell_theo = 0;
    double quote_theo0_bp = 0;
    double quote_theo0_sp = 0;
    double offset = 0;
    double q_offset = 0;
    double b_offset = 0;
    double s_offset = 0;
    double b_q_offset = 0;
    double s_q_offset = 0;
};

struct StrategyContext {
    const std::string *name = nullptr;
    const MSMarketDataField *last_ob;
    const MSMarketDataField *curr_ob;
    double mid_p;
    Theo theo;
    float prediction_raw;
    float prediction;
    int32_t position_limit;
    int32_t pi = 0;
    int32_t pe = 0;
    int32_t vl_pos = 0;
    int32_t vs_pos = 0;
    int32_t cum_buy = 0;
    int32_t cum_sell = 0;
    bool CanBuy() {
        return vs_pos == 0;
    }
    bool CanSell() {
        return vl_pos == 0;
    }
    uint64_t hit_buy_id;
    uint64_t hit_sell_id;
    uint64_t quote_buy_id;
    uint64_t quote_sell_id;
    uint64_t cancel_buy_id;
    uint64_t cancel_sell_id;
};

struct StrategyContextBse {
    const std::string *name = nullptr;
    const BseSnapshotField *last_ob;
    const BseSnapshotField *curr_ob;
    double mid_p;
    Theo theo;
    float prediction_raw;
    float prediction;
    int32_t static_pos;
    int32_t static_short_pos;
    int32_t position_limit;
    int32_t shortable;
    int32_t cur_shortable = 10000;
    int32_t pi = 0;
    int32_t pe = 0;
    int32_t vl_pos = 0;
    int32_t vs_pos = 0;
    int32_t cum_buy = 0;
    int32_t cum_sell = 0;
    bool CanBuy() {
        return vs_pos == 0;
    }
    bool CanSell() {
        return vl_pos == 0;
    }
    uint64_t hit_buy_id;
    uint64_t hit_sell_id;
    uint64_t quote_buy_id;
    uint64_t quote_sell_id;
    uint64_t cancel_buy_id;
    uint64_t cancel_sell_id;
};

#endif //COMMON_H
