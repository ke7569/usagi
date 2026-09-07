//
// Created by Administrator on 25-9-11.
//

#ifndef FACTOR_H
#define FACTOR_H
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iostream>

#include "common/contracts/legacy/LFDataStruct.h"
#include "common/contracts/legacy/RawDataStruct.h"

enum FactorType {
    OrderBook,
    OrderFlow
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


static double clip(double value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static double MP(const MSMarketDataField* ob) {
    return ob->AskVolume1 * ob->BidVolume1 > 1 ? (ob->AskPrice1 + ob->BidPrice1) / 2 : ob->LastPrice;
}

static bool isValid(const MSMarketDataField *ob) {
    return ob->AskVolume1 > 0 || ob->BidVolume1 > 0;
}

static double weighted_cross_price(const MSMarketDataField *cur_ob) {
    double sum = 0;
    int div = 0;
    do {
        if (__glibc_unlikely(cur_ob->AskVolume1 == 0 || cur_ob->BidVolume1 == 0)) {
            sum = cur_ob->LastPrice;
            div = 1;
            break;
        }
        sum += (cur_ob->AskPrice1*cur_ob->BidVolume1 + cur_ob->BidPrice1*cur_ob->AskVolume1) / (cur_ob->AskVolume1 + cur_ob->BidVolume1) * 5;
        div += 5;
        if (__glibc_unlikely(cur_ob->AskVolume2 == 0 || cur_ob->BidVolume2 == 0)) {
            break;
        }
        sum += (cur_ob->AskPrice2*cur_ob->BidVolume2 + cur_ob->BidPrice2*cur_ob->AskVolume2) / (cur_ob->AskVolume2 + cur_ob->BidVolume2) * 4;
        div += 4;
        if (__glibc_unlikely(cur_ob->AskVolume3 == 0 || cur_ob->BidVolume3 == 0)) {
            break;
        }
        sum += (cur_ob->AskPrice3*cur_ob->BidVolume3 + cur_ob->BidPrice3*cur_ob->AskVolume3)/(cur_ob->AskVolume3 + cur_ob->BidVolume3) * 3;
        div += 3;
        if (__glibc_unlikely(cur_ob->AskVolume4 == 0 || cur_ob->BidVolume4 == 0)) {
            break;
        }
        sum += (cur_ob->AskPrice4*cur_ob->BidVolume4 + cur_ob->BidPrice4*cur_ob->AskVolume4)/(cur_ob->AskVolume4 + cur_ob->BidVolume4) * 2;
        div += 2;
        if (__glibc_unlikely(cur_ob->AskVolume5 == 0 || cur_ob->BidVolume5 == 0)) {
            break;
        }
        sum += (cur_ob->AskPrice5*cur_ob->BidVolume5 + cur_ob->BidPrice5*cur_ob->AskVolume5)/(cur_ob->AskVolume5 + cur_ob->BidVolume5) * 1;
        div += 1;
    } while (0);
    return sum / div;
}

static double weighted_cross_price_rtn(const MSMarketDataField *cur_ob) {
    double mid = MP(cur_ob);
    double cross_price = weighted_cross_price(cur_ob);
    double ret =(cross_price / mid - 1) * 1e3;
    if (ret < -5) {
        ret = -5;
    } else if (ret > 5) {
        ret = 5;
    }
    return ret;
}

static double sqrt_trade_ratio(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob,double history_amount) {
    int64_t vol = cur_ob->Turnover - last_ob->Turnover;
    if (vol == 0) {
        return 0;
    }
    if (__glibc_unlikely(cur_ob->AskVolume1 == 0 || cur_ob->BidVolume1 == 0)){
        return 0;
    }
    if (__glibc_unlikely(last_ob->AskVolume1 == 0 || cur_ob->BidVolume1 == 0)){
        return 0;
    }
    double fsv = vol / history_amount * 1e3;
    double atp = (cur_ob->Turnover - last_ob->Turnover)/(cur_ob->Volume - last_ob->Volume);
    double spread = last_ob->AskPrice1 - last_ob->BidPrice1 ;
    double mid = MP(last_ob);
    double r = (atp - mid) / spread;
    if (r < -1){
        r = -1;
    }else if (r > 1){
        r = 1;
    }
    return sqrt(fsv * (1 + r)) - sqrt(fsv * (1 - r));
}


static double spread(const MSMarketDataField *cur_ob) {
    double mid = MP(cur_ob);
    double spread = (cur_ob->AskPrice1 - cur_ob->BidPrice1) / mid * 1e3 ;
    return spread;
}

static double mid_rtn(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
    double mid = MP(cur_ob);
    double last_mid = MP(last_ob);
    double rtn = (mid - last_mid) / last_mid * 1e3;
    return rtn;
}

static double tick_size(const MSMarketDataField *cur_ob) {
    return sqrt(cur_ob->LastPrice * 0.15);
}

static double bid_vol_change_ratio(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
    double vol = cur_ob->BidVolume1 + cur_ob->AskVolume1;;
    if (__glibc_unlikely(vol == 0)) return 0;
    double rt = 0;
    do {
      if (cur_ob->BidPrice1 - last_ob->BidPrice1 < -1e-6) {
        rt = -last_ob->BidVolume1 / vol;
        break;
      }
      if (cur_ob->BidPrice1 - last_ob->BidPrice1 > 1e-6) {
  
        rt = (last_ob->AskVolume1 + cur_ob->BidVolume1) / vol;
        break;
      }
  
      rt = (cur_ob->BidVolume1 - last_ob->BidVolume1) / vol;
  
    } while (0);

    return clip(rt,-100,100);
}

static double ask_vol_change_ratio(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
    double vol = cur_ob->BidVolume1 + cur_ob->AskVolume1;;
    if (__glibc_unlikely(vol == 0)) return 0;
    double rt = 0;
    do {
        if (cur_ob->AskPrice1 - last_ob->AskPrice1 > 1e-6) {
            rt = -last_ob->AskVolume1 / vol;
            break;
        }
        if (cur_ob->AskPrice1 - last_ob->AskPrice1 < -1e-6) {

            rt = (last_ob->BidVolume1 + cur_ob->AskVolume1) / vol;
            break;
        }

        rt = (cur_ob->AskVolume1 - last_ob->AskVolume1) / vol;

    } while (0);

    return clip(rt,-100,100);
}

static double weighted_price(const MSMarketDataField *cur_ob,int level=1) {
    double wp;
    double weight_sum = 0;
    double weight = 0;
    if (__glibc_unlikely(cur_ob->AskVolume1 + cur_ob->BidVolume1 == 0)) {
        return MP(cur_ob);
    }

        if (level >= 1) {
            weight_sum += (cur_ob->AskPrice1 * cur_ob->AskVolume1 + cur_ob->BidPrice1 * cur_ob->BidVolume1);
            weight += cur_ob->AskVolume1 + cur_ob->BidVolume1;
        }
        if (level >= 2) {
            weight_sum += cur_ob->AskPrice2 * cur_ob->AskVolume2 + cur_ob->BidPrice2 * cur_ob->BidVolume2;
            weight += cur_ob->AskVolume2 + cur_ob->BidVolume2;
        }
        if (level >= 3) {
            weight_sum += cur_ob->AskPrice3 * cur_ob->AskVolume3 + cur_ob->BidPrice3 * cur_ob->BidVolume3;
            weight += cur_ob->AskVolume3 + cur_ob->BidVolume3;
        }
        if (level >= 4) {
            weight_sum += cur_ob->AskPrice4 * cur_ob->AskVolume4 + cur_ob->BidPrice4 * cur_ob->BidVolume4;
            weight += cur_ob->AskVolume4 + cur_ob->BidVolume4;
        }
        if (level >= 5) {
            weight_sum += cur_ob->AskPrice5 * cur_ob->AskVolume5 + cur_ob->BidPrice5 * cur_ob->BidVolume5;
            weight += cur_ob->AskVolume5 + cur_ob->BidVolume5;
        }

    if (weight == 0) return MP(cur_ob);
    wp = weight_sum / weight;
    return wp;
}

static double weighted_rtn_lv1(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {

    return (weighted_price(cur_ob,1) - weighted_price(last_ob,1))/MP(cur_ob) * 1e3;
}

static double weighted_rtn_lv2(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
    return (weighted_price(cur_ob,2) - weighted_price(last_ob,2))/MP(cur_ob) * 1e3;
}

static double weighted_rtn_lv3(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
    return (weighted_price(cur_ob,3) - weighted_price(last_ob,3))/MP(cur_ob) * 1e3;
}

static double weighted_rtn_lv4(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
    return (weighted_price(cur_ob,4) - weighted_price(last_ob,4))/MP(cur_ob) * 1e3;
}

static double weighted_rtn_lv5(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
    return (weighted_price(cur_ob,5) - weighted_price(last_ob,5)) /MP(cur_ob) * 1e3;
}


static double weighted_ask_price(const MSMarketDataField *cur_ob) {
    int64_t ask_sum = cur_ob->AskVolume1 + cur_ob->AskVolume2 + cur_ob->AskVolume3 + cur_ob->AskVolume4 + cur_ob->AskVolume5;
    if (ask_sum == 0) return 0;
    double weight = 0;
    double mid = MP(cur_ob);
    do {
        if (__glibc_unlikely(cur_ob->AskVolume1 == 0)) break;
        weight += cur_ob->AskPrice1 * cur_ob->AskVolume1;
        if (__glibc_unlikely(cur_ob->AskVolume2 == 0)) break;
        weight += cur_ob->AskPrice2 * cur_ob->AskVolume2;
        if (__glibc_unlikely(cur_ob->AskVolume3 == 0)) break;
        weight += cur_ob->AskPrice3 * cur_ob->AskVolume3;
        if (__glibc_unlikely(cur_ob->AskVolume4 == 0)) break;
        weight += cur_ob->AskPrice4 * cur_ob->AskVolume4;
        if (__glibc_unlikely(cur_ob->AskVolume5 == 0)) break;
        weight += cur_ob->AskPrice5 * cur_ob->AskVolume5;
    } while (0);
    return (weight / ask_sum - mid) ;
}

static double weighted_bid_price(const MSMarketDataField *cur_ob) {
    int64_t bid_sum = cur_ob->BidVolume1 + cur_ob->BidVolume2 + cur_ob->BidVolume3 + cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (bid_sum == 0) return 0;
    double weight = 0;
    double mid = MP(cur_ob);
    do {
        if (__glibc_unlikely(cur_ob->BidVolume1 == 0)) break;
        weight += cur_ob->BidPrice1 * cur_ob->BidVolume1;
        if (__glibc_unlikely(cur_ob->BidVolume2 == 0)) break;
        weight += cur_ob->BidPrice2 * cur_ob->BidVolume2;
        if (__glibc_unlikely(cur_ob->BidVolume3 == 0)) break;
        weight += cur_ob->BidPrice3 * cur_ob->BidVolume3;
        if (__glibc_unlikely(cur_ob->BidVolume4 == 0)) break;
        weight += cur_ob->BidPrice4 * cur_ob->BidVolume4;
        if (__glibc_unlikely(cur_ob->BidVolume5 == 0)) break;
        weight += cur_ob->BidPrice5 * cur_ob->BidVolume5;
    } while (0);
    return (mid - weight / bid_sum);
}

static double weighted_ask(const MSMarketDataField *cur_ob) {
    int64_t ask_sum = cur_ob->AskVolume1 + cur_ob->AskVolume2 + cur_ob->AskVolume3 + cur_ob->AskVolume4 + cur_ob->AskVolume5;
    if (ask_sum == 0) return 0;
    double weight = 0;
    double mid = MP(cur_ob);
    do {
        if (__glibc_unlikely(cur_ob->AskVolume1 == 0)) break;
        weight += cur_ob->AskPrice1 * cur_ob->AskVolume1;
        if (__glibc_unlikely(cur_ob->AskVolume2 == 0)) break;
        weight += cur_ob->AskPrice2 * cur_ob->AskVolume2;
        if (__glibc_unlikely(cur_ob->AskVolume3 == 0)) break;
        weight += cur_ob->AskPrice3 * cur_ob->AskVolume3;
        if (__glibc_unlikely(cur_ob->AskVolume4 == 0)) break;
        weight += cur_ob->AskPrice4 * cur_ob->AskVolume4;
        if (__glibc_unlikely(cur_ob->AskVolume5 == 0)) break;
        weight += cur_ob->AskPrice5 * cur_ob->AskVolume5;
    } while (0);
    return (weight / ask_sum - mid) / mid * 1e3;
}

static double weighted_bid(const MSMarketDataField *cur_ob) {
    int64_t bid_sum = cur_ob->BidVolume1 + cur_ob->BidVolume2 + cur_ob->BidVolume3 + cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (bid_sum == 0) return 0;
    double weight = 0;
    double mid = MP(cur_ob);
    do {
        if (__glibc_unlikely(cur_ob->BidVolume1 == 0)) break;
        weight += cur_ob->BidPrice1 * cur_ob->BidVolume1;
        if (__glibc_unlikely(cur_ob->BidVolume2 == 0)) break;
        weight += cur_ob->BidPrice2 * cur_ob->BidVolume2;
        if (__glibc_unlikely(cur_ob->BidVolume3 == 0)) break;
        weight += cur_ob->BidPrice3 * cur_ob->BidVolume3;
        if (__glibc_unlikely(cur_ob->BidVolume4 == 0)) break;
        weight += cur_ob->BidPrice4 * cur_ob->BidVolume4;
        if (__glibc_unlikely(cur_ob->BidVolume5 == 0)) break;
        weight += cur_ob->BidPrice5 * cur_ob->BidVolume5;
    } while (0);
    return weight / bid_sum;
}

static double pct_weighted_bid(const MSMarketDataField *cur_ob) {
    return weighted_bid_price(cur_ob) / MP(cur_ob) * 1e3;
}

static double pct_weighted_ask(const MSMarketDataField *cur_ob) {
    return weighted_ask_price(cur_ob) / MP(cur_ob) * 1e3;
}

static double weighted_ask_rtn(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
    return (weighted_ask_price(cur_ob) - weighted_ask_price(last_ob)) / MP(cur_ob)  * 1e3;
}

static double weighted_bid_rtn(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
        return (weighted_bid_price(cur_ob) - weighted_bid_price(last_ob)) / MP(cur_ob)  * 1e3;
}

static double weighted_imabalance_lv5(const MSMarketDataField *cur_ob) {
    double ask_sum = 5*cur_ob->AskVolume1 + 4*cur_ob->AskVolume2 + 3*cur_ob->AskVolume3 + 2*cur_ob->AskVolume4 + cur_ob->AskVolume5;
    double bid_sum =  5*cur_ob->BidVolume1 + 4*cur_ob->BidVolume2 + 3*cur_ob->BidVolume3 + 2*cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (__glibc_unlikely(ask_sum == 0 || bid_sum == 0)) return 0;
    return  - (bid_sum - ask_sum) / (ask_sum + bid_sum) / 2;
}

static double imbalance_lv5(const MSMarketDataField *cur_ob) {
    double ask_sum = cur_ob->AskVolume1 + cur_ob->AskVolume2 + cur_ob->AskVolume3 + cur_ob->AskVolume4 + cur_ob->AskVolume5;
    double bid_sum =  cur_ob->BidVolume1 + cur_ob->BidVolume2 + cur_ob->BidVolume3 + cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (__glibc_unlikely(ask_sum == 0 || bid_sum == 0)) return 0;
    return - (bid_sum - ask_sum) / (ask_sum + bid_sum) / 2;
}

static double pct_turnover(const MSMarketDataField *cur_ob,const MSMarketDataField *last_ob) {
    double quote_sum = cur_ob->AskVolume1 * cur_ob->AskPrice1 + cur_ob->AskVolume2 * cur_ob->AskPrice2
    + cur_ob->AskVolume3*cur_ob->AskPrice3 + cur_ob->AskVolume4 * cur_ob->AskPrice4 + cur_ob->AskVolume5*cur_ob->AskPrice5 +
        cur_ob->BidVolume1 * cur_ob->BidPrice1 + cur_ob->BidVolume2 * cur_ob->BidPrice2
    + cur_ob->BidVolume3*cur_ob->BidPrice3 + cur_ob->BidVolume4 * cur_ob->BidPrice4 + cur_ob->BidVolume5*cur_ob->BidPrice5;

    if (quote_sum == 0) return 0;
    return (cur_ob->Turnover - last_ob->Turnover) / quote_sum;
}

static double ask_pct_lv1(const MSMarketDataField *cur_ob) {
    double ask_sum =  cur_ob->AskVolume1 + cur_ob->AskVolume2 + cur_ob->AskVolume3 + cur_ob->AskVolume4 + cur_ob->AskVolume5;
    if (__glibc_unlikely(ask_sum == 0)) return 0;
    return cur_ob->AskVolume1 / ask_sum;
}

static double bid_pct_lv1(const MSMarketDataField *cur_ob) {
    double bid_sum =  cur_ob->BidVolume1 + cur_ob->BidVolume2 + cur_ob->BidVolume3 + cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (__glibc_unlikely(bid_sum == 0)) return 0;
    return cur_ob->BidVolume1 / bid_sum;
}



/***
 Bse版本
 ****/

static double MP(const BseSnapshotField* ob) {
    return ob->AskVolume1 * ob->BidVolume1 > 1 ? (ob->AskPrice1 + ob->BidPrice1) / 2 : ob->LastPrice;
}

static bool isValid(const BseSnapshotField *ob) {
    return ob->AskVolume1 > 0 || ob->BidVolume1 > 0;
}

static double weighted_cross_price(const BseSnapshotField *cur_ob) {
    double sum = 0;
    int div = 0;
    do {
        if (__glibc_unlikely(cur_ob->AskVolume1 == 0 || cur_ob->BidVolume1 == 0)) {
            sum = cur_ob->LastPrice;
            div = 1;
            break;
        }
        sum += (cur_ob->AskPrice1*cur_ob->BidVolume1 + cur_ob->BidPrice1*cur_ob->AskVolume1) / (cur_ob->AskVolume1 + cur_ob->BidVolume1) * 5;
        div += 5;
        if (__glibc_unlikely(cur_ob->AskVolume2 == 0 || cur_ob->BidVolume2 == 0)) {
            break;
        }
        sum += (cur_ob->AskPrice2*cur_ob->BidVolume2 + cur_ob->BidPrice2*cur_ob->AskVolume2) / (cur_ob->AskVolume2 + cur_ob->BidVolume2) * 4;
        div += 4;
        if (__glibc_unlikely(cur_ob->AskVolume3 == 0 || cur_ob->BidVolume3 == 0)) {
            break;
        }
        sum += (cur_ob->AskPrice3*cur_ob->BidVolume3 + cur_ob->BidPrice3*cur_ob->AskVolume3)/(cur_ob->AskVolume3 + cur_ob->BidVolume3) * 3;
        div += 3;
        if (__glibc_unlikely(cur_ob->AskVolume4 == 0 || cur_ob->BidVolume4 == 0)) {
            break;
        }
        sum += (cur_ob->AskPrice4*cur_ob->BidVolume4 + cur_ob->BidPrice4*cur_ob->AskVolume4)/(cur_ob->AskVolume4 + cur_ob->BidVolume4) * 2;
        div += 2;
        if (__glibc_unlikely(cur_ob->AskVolume5 == 0 || cur_ob->BidVolume5 == 0)) {
            break;
        }
        sum += (cur_ob->AskPrice5*cur_ob->BidVolume5 + cur_ob->BidPrice5*cur_ob->AskVolume5)/(cur_ob->AskVolume5 + cur_ob->BidVolume5) * 1;
        div += 1;
    } while (0);
    return sum / div;
}

static double weighted_cross_price_rtn(const BseSnapshotField *cur_ob) {
    double mid = MP(cur_ob);
    double cross_price = weighted_cross_price(cur_ob);
    double ret =(cross_price / mid - 1) * 1e3;
    if (ret < -5) {
        ret = -5;
    } else if (ret > 5) {
        ret = 5;
    }
    return ret;
}

static double sqrt_trade_ratio(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob,double history_amount) {
    history_amount = std::max(history_amount, 5e6);
    const double turnover = cur_ob->Turnover - last_ob->Turnover;
    if (turnover < 0) {
        return 0;
    }
    if (std::abs(turnover) < 1e-5) {
        return 0;
    }
    if (__glibc_unlikely(cur_ob->AskVolume1 == 0 || cur_ob->BidVolume1 == 0)){
        return 0;
    }
    if (__glibc_unlikely(last_ob->AskVolume1 == 0 || last_ob->BidVolume1 == 0)){
        return 0;
    }
    if (__glibc_unlikely(std::abs(cur_ob->BidPrice1 - DBL_MAX) < 1 ||
                         std::abs(cur_ob->AskPrice1 - DBL_MAX) < 1 ||
                         std::abs(last_ob->BidPrice1 - DBL_MAX) < 1 ||
                         std::abs(last_ob->AskPrice1 - DBL_MAX) < 1)){
        return 0;
    }
    const double volume_diff = cur_ob->Volume - last_ob->Volume;
    if (std::abs(volume_diff) < 1e-5) {
        return 0;
    }
    double fsv = turnover / history_amount * 1e3;
    double atp = turnover / volume_diff;
    double spread = last_ob->AskPrice1 - last_ob->BidPrice1 ;
    if (std::abs(spread) < 1e-6) {
        return 0;
    }
    double mid = MP(last_ob);
    double r = (atp - mid) / spread;
    if (r < -1){
        r = -1;
    }else if (r > 1){
        r = 1;
    }
    return sqrt(fsv * (1 + r)) - sqrt(fsv * (1 - r));
}


static double spread(const BseSnapshotField *cur_ob) {
    double mid = MP(cur_ob);
    double spread = (cur_ob->AskPrice1 - cur_ob->BidPrice1) / mid * 1e3 ;
    return spread;
}

static double mid_rtn(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
    double mid = MP(cur_ob);
    double last_mid = MP(last_ob);
    double rtn = (mid - last_mid) / last_mid * 1e3;
    return rtn;
}

static double tick_size(const BseSnapshotField *cur_ob) {
    return sqrt(cur_ob->LastPrice * 0.15);
}

static double bid_vol_change_ratio(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
    double vol = cur_ob->BidVolume1 + cur_ob->AskVolume1;;
    if (__glibc_unlikely(vol == 0)) return 0;
    double rt = 0;
    do {
      if (cur_ob->BidPrice1 - last_ob->BidPrice1 < -1e-6) {
        rt = -last_ob->BidVolume1 / vol;
        break;
      }
      if (cur_ob->BidPrice1 - last_ob->BidPrice1 > 1e-6) {

        rt = (last_ob->AskVolume1 + cur_ob->BidVolume1) / vol;
        break;
      }

      rt = (cur_ob->BidVolume1 - last_ob->BidVolume1) / vol;

    } while (0);

    return clip(rt,-100,100);
}

static double ask_vol_change_ratio(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
    double vol = cur_ob->BidVolume1 + cur_ob->AskVolume1;;
    if (__glibc_unlikely(vol == 0)) return 0;
    double rt = 0;
    do {
        if (cur_ob->AskPrice1 - last_ob->AskPrice1 > 1e-6) {
            rt = -last_ob->AskVolume1 / vol;
            break;
        }
        if (cur_ob->AskPrice1 - last_ob->AskPrice1 < -1e-6) {

            rt = (last_ob->BidVolume1 + cur_ob->AskVolume1) / vol;
            break;
        }

        rt = (cur_ob->AskVolume1 - last_ob->AskVolume1) / vol;

    } while (0);

    return clip(rt,-100,100);
}

static double weighted_price(const BseSnapshotField *cur_ob,int level=1) {
    double wp;
    double weight_sum = 0;
    double weight = 0;
    if (__glibc_unlikely(cur_ob->AskVolume1 + cur_ob->BidVolume1 == 0)) {
        return MP(cur_ob);
    }

        if (level >= 1) {
            weight_sum += (cur_ob->AskPrice1 * cur_ob->AskVolume1 + cur_ob->BidPrice1 * cur_ob->BidVolume1);
            weight += cur_ob->AskVolume1 + cur_ob->BidVolume1;
        }
        if (level >= 2) {
            weight_sum += cur_ob->AskPrice2 * cur_ob->AskVolume2 + cur_ob->BidPrice2 * cur_ob->BidVolume2;
            weight += cur_ob->AskVolume2 + cur_ob->BidVolume2;
        }
        if (level >= 3) {
            weight_sum += cur_ob->AskPrice3 * cur_ob->AskVolume3 + cur_ob->BidPrice3 * cur_ob->BidVolume3;
            weight += cur_ob->AskVolume3 + cur_ob->BidVolume3;
        }
        if (level >= 4) {
            weight_sum += cur_ob->AskPrice4 * cur_ob->AskVolume4 + cur_ob->BidPrice4 * cur_ob->BidVolume4;
            weight += cur_ob->AskVolume4 + cur_ob->BidVolume4;
        }
        if (level >= 5) {
            weight_sum += cur_ob->AskPrice5 * cur_ob->AskVolume5 + cur_ob->BidPrice5 * cur_ob->BidVolume5;
            weight += cur_ob->AskVolume5 + cur_ob->BidVolume5;
        }

    if (weight == 0) return MP(cur_ob);
    wp = weight_sum / weight;
    return wp;
}

static double weighted_rtn_lv1(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {

    return (weighted_price(cur_ob,1) - weighted_price(last_ob,1))/MP(cur_ob) * 1e3;
}

static double weighted_rtn_lv2(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
    return (weighted_price(cur_ob,2) - weighted_price(last_ob,2))/MP(cur_ob) * 1e3;
}

static double weighted_rtn_lv3(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
    return (weighted_price(cur_ob,3) - weighted_price(last_ob,3))/MP(cur_ob) * 1e3;
}

static double weighted_rtn_lv4(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
    return (weighted_price(cur_ob,4) - weighted_price(last_ob,4))/MP(cur_ob) * 1e3;
}

static double weighted_rtn_lv5(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
    return (weighted_price(cur_ob,5) - weighted_price(last_ob,5)) /MP(cur_ob) * 1e3;
}


static double weighted_ask_price(const BseSnapshotField *cur_ob) {
    int64_t ask_sum = cur_ob->AskVolume1 + cur_ob->AskVolume2 + cur_ob->AskVolume3 + cur_ob->AskVolume4 + cur_ob->AskVolume5;
    if (ask_sum == 0) return 0;
    double weight = 0;
    double mid = MP(cur_ob);
    do {
        if (__glibc_unlikely(cur_ob->AskVolume1 == 0)) break;
        weight += cur_ob->AskPrice1 * cur_ob->AskVolume1;
        if (__glibc_unlikely(cur_ob->AskVolume2 == 0)) break;
        weight += cur_ob->AskPrice2 * cur_ob->AskVolume2;
        if (__glibc_unlikely(cur_ob->AskVolume3 == 0)) break;
        weight += cur_ob->AskPrice3 * cur_ob->AskVolume3;
        if (__glibc_unlikely(cur_ob->AskVolume4 == 0)) break;
        weight += cur_ob->AskPrice4 * cur_ob->AskVolume4;
        if (__glibc_unlikely(cur_ob->AskVolume5 == 0)) break;
        weight += cur_ob->AskPrice5 * cur_ob->AskVolume5;
    } while (0);
    return (weight / ask_sum - mid) ;
}

static double weighted_bid_price(const BseSnapshotField *cur_ob) {
    int64_t bid_sum = cur_ob->BidVolume1 + cur_ob->BidVolume2 + cur_ob->BidVolume3 + cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (bid_sum == 0) return 0;
    double weight = 0;
    double mid = MP(cur_ob);
    do {
        if (__glibc_unlikely(cur_ob->BidVolume1 == 0)) break;
        weight += cur_ob->BidPrice1 * cur_ob->BidVolume1;
        if (__glibc_unlikely(cur_ob->BidVolume2 == 0)) break;
        weight += cur_ob->BidPrice2 * cur_ob->BidVolume2;
        if (__glibc_unlikely(cur_ob->BidVolume3 == 0)) break;
        weight += cur_ob->BidPrice3 * cur_ob->BidVolume3;
        if (__glibc_unlikely(cur_ob->BidVolume4 == 0)) break;
        weight += cur_ob->BidPrice4 * cur_ob->BidVolume4;
        if (__glibc_unlikely(cur_ob->BidVolume5 == 0)) break;
        weight += cur_ob->BidPrice5 * cur_ob->BidVolume5;
    } while (0);
    return (mid - weight / bid_sum);
}

static double weighted_ask(const BseSnapshotField *cur_ob) {
    int64_t ask_sum = cur_ob->AskVolume1 + cur_ob->AskVolume2 + cur_ob->AskVolume3 + cur_ob->AskVolume4 + cur_ob->AskVolume5;
    if (ask_sum == 0) return 0;
    double weight = 0;
    double mid = MP(cur_ob);
    do {
        if (__glibc_unlikely(cur_ob->AskVolume1 == 0)) break;
        weight += cur_ob->AskPrice1 * cur_ob->AskVolume1;
        if (__glibc_unlikely(cur_ob->AskVolume2 == 0)) break;
        weight += cur_ob->AskPrice2 * cur_ob->AskVolume2;
        if (__glibc_unlikely(cur_ob->AskVolume3 == 0)) break;
        weight += cur_ob->AskPrice3 * cur_ob->AskVolume3;
        if (__glibc_unlikely(cur_ob->AskVolume4 == 0)) break;
        weight += cur_ob->AskPrice4 * cur_ob->AskVolume4;
        if (__glibc_unlikely(cur_ob->AskVolume5 == 0)) break;
        weight += cur_ob->AskPrice5 * cur_ob->AskVolume5;
    } while (0);
    return (weight / ask_sum - mid) / mid * 1e3;
}

static double weighted_bid(const BseSnapshotField *cur_ob) {
    int64_t bid_sum = cur_ob->BidVolume1 + cur_ob->BidVolume2 + cur_ob->BidVolume3 + cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (bid_sum == 0) return 0;
    double weight = 0;
    double mid = MP(cur_ob);
    do {
        if (__glibc_unlikely(cur_ob->BidVolume1 == 0)) break;
        weight += cur_ob->BidPrice1 * cur_ob->BidVolume1;
        if (__glibc_unlikely(cur_ob->BidVolume2 == 0)) break;
        weight += cur_ob->BidPrice2 * cur_ob->BidVolume2;
        if (__glibc_unlikely(cur_ob->BidVolume3 == 0)) break;
        weight += cur_ob->BidPrice3 * cur_ob->BidVolume3;
        if (__glibc_unlikely(cur_ob->BidVolume4 == 0)) break;
        weight += cur_ob->BidPrice4 * cur_ob->BidVolume4;
        if (__glibc_unlikely(cur_ob->BidVolume5 == 0)) break;
        weight += cur_ob->BidPrice5 * cur_ob->BidVolume5;
    } while (0);
    return weight / bid_sum;
}

static double pct_weighted_bid(const BseSnapshotField *cur_ob) {
    return weighted_bid_price(cur_ob) / MP(cur_ob) * 1e3;
}

static double pct_weighted_ask(const BseSnapshotField *cur_ob) {
    return weighted_ask_price(cur_ob) / MP(cur_ob) * 1e3;
}

static double weighted_ask_rtn(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
    return (weighted_ask_price(cur_ob) - weighted_ask_price(last_ob)) / MP(cur_ob)  * 1e3;
}

static double weighted_bid_rtn(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
        return (weighted_bid_price(cur_ob) - weighted_bid_price(last_ob)) / MP(cur_ob)  * 1e3;
}

static double weighted_imabalance_lv5(const BseSnapshotField *cur_ob) {
    double ask_sum = 5*cur_ob->AskVolume1 + 4*cur_ob->AskVolume2 + 3*cur_ob->AskVolume3 + 2*cur_ob->AskVolume4 + cur_ob->AskVolume5;
    double bid_sum =  5*cur_ob->BidVolume1 + 4*cur_ob->BidVolume2 + 3*cur_ob->BidVolume3 + 2*cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (__glibc_unlikely(ask_sum == 0 || bid_sum == 0)) return 0;
    return  - (bid_sum - ask_sum) / (ask_sum + bid_sum) / 2;
}

static double imbalance_lv5(const BseSnapshotField *cur_ob) {
    double ask_sum = cur_ob->AskVolume1 + cur_ob->AskVolume2 + cur_ob->AskVolume3 + cur_ob->AskVolume4 + cur_ob->AskVolume5;
    double bid_sum =  cur_ob->BidVolume1 + cur_ob->BidVolume2 + cur_ob->BidVolume3 + cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (__glibc_unlikely(ask_sum == 0 || bid_sum == 0)) return 0;
    return - (bid_sum - ask_sum) / (ask_sum + bid_sum) / 2;
}

static double pct_turnover(const BseSnapshotField *cur_ob,const BseSnapshotField *last_ob) {
    double quote_sum = cur_ob->AskVolume1 * cur_ob->AskPrice1 + cur_ob->AskVolume2 * cur_ob->AskPrice2
    + cur_ob->AskVolume3*cur_ob->AskPrice3 + cur_ob->AskVolume4 * cur_ob->AskPrice4 + cur_ob->AskVolume5*cur_ob->AskPrice5 +
        cur_ob->BidVolume1 * cur_ob->BidPrice1 + cur_ob->BidVolume2 * cur_ob->BidPrice2
    + cur_ob->BidVolume3*cur_ob->BidPrice3 + cur_ob->BidVolume4 * cur_ob->BidPrice4 + cur_ob->BidVolume5*cur_ob->BidPrice5;

    if (quote_sum == 0) return 0;
    return (cur_ob->Turnover - last_ob->Turnover) / quote_sum;
}

static double ask_pct_lv1(const BseSnapshotField *cur_ob) {
    double ask_sum =  cur_ob->AskVolume1 + cur_ob->AskVolume2 + cur_ob->AskVolume3 + cur_ob->AskVolume4 + cur_ob->AskVolume5;
    if (__glibc_unlikely(ask_sum == 0)) return 0;
    return cur_ob->AskVolume1 / ask_sum;
}

static double bid_pct_lv1(const BseSnapshotField *cur_ob) {
    double bid_sum =  cur_ob->BidVolume1 + cur_ob->BidVolume2 + cur_ob->BidVolume3 + cur_ob->BidVolume4 + cur_ob->BidVolume5;
    if (__glibc_unlikely(bid_sum == 0)) return 0;
    return cur_ob->BidVolume1 / bid_sum;
}
#endif //FACTOR_H
