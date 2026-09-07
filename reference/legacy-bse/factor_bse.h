//
// Created by Administrator on 25-11-25.
//

#ifndef FACTOR_BSE_H
#define FACTOR_BSE_H



#include <cmath>
#include <iostream>

#include "common/contracts/legacy/LFDataStruct.h"
#include "common/contracts/legacy/RawDataStruct.h"

enum FactorType {
    OrderBook,
    OrderFlow
  };


struct Factor {
    float weight_cross_price;
    float sqrt_trade_ratio;
    float spread;
    float mid_rtn;
    float tick_size;
    float bid_vol_change_ratio;
    float ask_vol_change_ratio;
    float weighted_rtn_lv1;
    float weighted_rtn_lv2;
    float weighted_rtn_lv3;
    float weighted_rtn_lv4;
    float weighted_rtn_lv5;
    float weighted_ask_price;
    float weighted_bid_price;

    Factor() = default;
    FactorType factor_type = OrderBook;
};

struct OrderBookFactor {
    float weighted_cross_price_rtn;
    float sqrt_trade_ratio;
    float spread;
    float mid_rtn;
    float tick_size;
    float bid_vol_change_ratio;
    float ask_vol_change_ratio;
    float weighted_rtn_lv1;
    float weighted_rtn_lv2;
    float weighted_rtn_lv3;
    float weighted_rtn_lv4;
    float weighted_rtn_lv5;
    float pct_weighted_ask;
    float pct_weighted_bid;
    float weighted_ask_rtn;
    float weighted_bid_rtn;
    float imbalance_lv5;
    float weighted_imabalance_lv5;
    float pct_turnover;
    float ask_pct_lv1;
    float bid_pct_lv1;
};

struct OrderflowFactor {
    float positive_order_flow = 0;
    float negative_order_flow = 0;
    float market_order_flow = 0;
    float cancel_buy_flow = 0;
    float cancel_sell_flow = 0;
    float positive_trade_flow = 0;
    float negative_trade_flow = 0;
    float positive_trade_num = 0;
    float negative_trade_num = 0;
};





#endif //FACTOR_BSE_H
