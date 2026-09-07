//
// Created by Administrator on 25-9-9.
//

#include "adapters/deepwin/legacy/predictor/predictor.h"

#include <iostream>
#include <ostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstdint>
#include <cmath>
#include "third_party/nlohmann/json.hpp"
#include "adapters/deepwin/legacy/bse_latency.h"
#include "common/market_data/legacy/shsz_full_orderbook_diagnostics.h"

namespace {

struct ShSzLegacyOrderflowTimingBreakdown {
    std::array<uint64_t, SHSZ_LEGACY_OF_FEATURE_COUNT> elapsed_ns = {};
};

void process_sze_cached_order_queue(const MSMarketDataField* last_ob,
                                    double price_tick,
                                    std::queue<LFL2OrderField>* queue,
                                    OrderflowFactor* factor,
                                    ShSzLegacyOrderflowTimingBreakdown* timing) {
    if (last_ob == nullptr || queue == nullptr || factor == nullptr) {
        return;
    }
    const bool latency_enabled = (timing != 0) && shsz_full_orderbook_latency_enabled();

    while (!queue->empty()) {
        const LFL2OrderField order = queue->front();
        queue->pop();
        {
            const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
            const float bench_price = last_ob->BidPrice1;
            const float limit_price = last_ob->AskVolume1 == 0 ? bench_price + price_tick : last_ob->AskPrice1;
            if (order.OrderKind[0] == 'B' && order.Price < limit_price - 1e-6) {
                const double weight = 1 - std::tanh((bench_price / order.Price - 1) * 100);
                factor->positive_order_flow += order.Volume * order.Price * weight;
            }
            if (latency_enabled) {
                timing->elapsed_ns[SHSZ_LEGACY_OF_POSITIVE_ORDER_FLOW] +=
                    (shsz_full_orderbook_now_ns() - begin_ns);
            }
        }
        {
            const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
            const float bench_price = last_ob->AskPrice1;
            const float limit_price = last_ob->BidVolume1 == 0 ? bench_price - price_tick : last_ob->BidPrice1;
            if (order.OrderKind[0] == 'S' && order.Price > limit_price + 1e-6) {
                const double weight = 1 - std::tanh((order.Price / bench_price - 1) * 100);
                factor->negative_order_flow += order.Volume * order.Price * weight;
            }
            if (latency_enabled) {
                timing->elapsed_ns[SHSZ_LEGACY_OF_NEGATIVE_ORDER_FLOW] +=
                    (shsz_full_orderbook_now_ns() - begin_ns);
            }
        }
        {
            const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
            if (order.OrderKind[0] == 'B') {
                if (order.OrdType[0] != '2' || order.Price > last_ob->AskPrice1 + 1e-6) {
                    factor->market_order_flow += order.Volume * order.Price;
                }
            } else {
                if (order.OrdType[0] != '2' || order.Price < last_ob->BidPrice1 - 1e-6) {
                    factor->market_order_flow -= order.Volume * order.Price;
                }
            }
            if (latency_enabled) {
                timing->elapsed_ns[SHSZ_LEGACY_OF_MARKET_ORDER_FLOW] +=
                    (shsz_full_orderbook_now_ns() - begin_ns);
            }
        }
    }
}

void process_sze_cached_trade_queue(const MSMarketDataField* last_ob,
                                    std::queue<LFL2TradeField>* queue,
                                    OrderflowFactor* factor,
                                    ShSzLegacyOrderflowTimingBreakdown* timing) {
    if (last_ob == nullptr || queue == nullptr || factor == nullptr) {
        return;
    }
    const bool latency_enabled = (timing != 0) && shsz_full_orderbook_latency_enabled();

    while (!queue->empty()) {
        const LFL2TradeField trade = queue->front();
        queue->pop();

        if (trade.OrderKind[0] == '4') {
            if (trade.OfferApplSeqNum == 0) {
                const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
                factor->cancel_buy_flow += trade.Volume * MP(last_ob);
                if (latency_enabled) {
                    timing->elapsed_ns[SHSZ_LEGACY_OF_CANCEL_BUY_FLOW] +=
                        (shsz_full_orderbook_now_ns() - begin_ns);
                }
            }
            if (trade.BidApplSeqNum == 0) {
                const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
                factor->cancel_sell_flow += trade.Volume * MP(last_ob);
                if (latency_enabled) {
                    timing->elapsed_ns[SHSZ_LEGACY_OF_CANCEL_SELL_FLOW] +=
                        (shsz_full_orderbook_now_ns() - begin_ns);
                }
            }
            continue;
        }

        if (trade.OrderKind[0] != 'F') {
            continue;
        }

        if (trade.BidApplSeqNum > trade.OfferApplSeqNum) {
            const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
            factor->positive_trade_flow += trade.Volume * trade.Price;
            factor->positive_trade_num += 1;
            if (latency_enabled) {
                const uint64_t elapsed_ns = shsz_full_orderbook_now_ns() - begin_ns;
                timing->elapsed_ns[SHSZ_LEGACY_OF_POSITIVE_TRADE_FLOW] += elapsed_ns;
                timing->elapsed_ns[SHSZ_LEGACY_OF_POSITIVE_TRADE_NUM] += elapsed_ns;
            }
        } else {
            const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
            factor->negative_trade_flow += trade.Volume * trade.Price;
            factor->negative_trade_num += 1;
            if (latency_enabled) {
                const uint64_t elapsed_ns = shsz_full_orderbook_now_ns() - begin_ns;
                timing->elapsed_ns[SHSZ_LEGACY_OF_NEGATIVE_TRADE_FLOW] += elapsed_ns;
                timing->elapsed_ns[SHSZ_LEGACY_OF_NEGATIVE_TRADE_NUM] += elapsed_ns;
            }
        }
    }
}

void normalize_sze_orderflow_factors(double history_amount,
                                     OrderflowFactor* factor,
                                     ShSzLegacyOrderflowTimingBreakdown* timing) {
    if (factor == nullptr) {
        return;
    }
    const bool latency_enabled = (timing != 0) && shsz_full_orderbook_latency_enabled();

    {
        const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
        factor->positive_order_flow = factor->positive_order_flow / history_amount * 1e3;
        if (latency_enabled) {
            timing->elapsed_ns[SHSZ_LEGACY_OF_POSITIVE_ORDER_FLOW] +=
                (shsz_full_orderbook_now_ns() - begin_ns);
        }
    }
    {
        const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
        factor->negative_order_flow = factor->negative_order_flow / history_amount * 1e3;
        if (latency_enabled) {
            timing->elapsed_ns[SHSZ_LEGACY_OF_NEGATIVE_ORDER_FLOW] +=
                (shsz_full_orderbook_now_ns() - begin_ns);
        }
    }
    {
        const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
        factor->market_order_flow = factor->market_order_flow / history_amount * 1e3;
        if (latency_enabled) {
            timing->elapsed_ns[SHSZ_LEGACY_OF_MARKET_ORDER_FLOW] +=
                (shsz_full_orderbook_now_ns() - begin_ns);
        }
    }
    {
        const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
        factor->cancel_buy_flow = factor->cancel_buy_flow / history_amount * 1e3;
        if (latency_enabled) {
            timing->elapsed_ns[SHSZ_LEGACY_OF_CANCEL_BUY_FLOW] +=
                (shsz_full_orderbook_now_ns() - begin_ns);
        }
    }
    {
        const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
        factor->cancel_sell_flow = factor->cancel_sell_flow / history_amount * 1e3;
        if (latency_enabled) {
            timing->elapsed_ns[SHSZ_LEGACY_OF_CANCEL_SELL_FLOW] +=
                (shsz_full_orderbook_now_ns() - begin_ns);
        }
    }
    {
        const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
        factor->positive_trade_flow = factor->positive_trade_flow / history_amount * 1e3;
        if (latency_enabled) {
            timing->elapsed_ns[SHSZ_LEGACY_OF_POSITIVE_TRADE_FLOW] +=
                (shsz_full_orderbook_now_ns() - begin_ns);
        }
    }
    {
        const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
        factor->negative_trade_flow = factor->negative_trade_flow / history_amount * 1e3;
        if (latency_enabled) {
            timing->elapsed_ns[SHSZ_LEGACY_OF_NEGATIVE_TRADE_FLOW] +=
                (shsz_full_orderbook_now_ns() - begin_ns);
        }
    }
    {
        const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
        factor->positive_trade_num = std::log(1 + factor->positive_trade_num);
        if (latency_enabled) {
            timing->elapsed_ns[SHSZ_LEGACY_OF_POSITIVE_TRADE_NUM] +=
                (shsz_full_orderbook_now_ns() - begin_ns);
        }
    }
    {
        const uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
        factor->negative_trade_num = std::log(1 + factor->negative_trade_num);
        if (latency_enabled) {
            timing->elapsed_ns[SHSZ_LEGACY_OF_NEGATIVE_TRADE_NUM] +=
                (shsz_full_orderbook_now_ns() - begin_ns);
        }
    }
}

void commit_sze_orderflow_timing(const ShSzLegacyOrderflowTimingBreakdown& timing) {
    if (!shsz_full_orderbook_latency_enabled()) {
        return;
    }
    ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
    for (size_t i = 0; i < timing.elapsed_ns.size(); ++i) {
        stats.add_legacy_orderflow_sample(i, timing.elapsed_ns[i]);
    }
}

} // namespace


// 静态成员变量定义
std::unordered_map<std::string, RealGRU*> PredictorBase::s_shared_real_gru_models_;
std::unordered_map<std::string, gru_ver_1*> PredictorBase::s_shared_gru_ver_1_models_;
std::unordered_map<std::string, BsePredictor::BseScalerCacheEntry> BsePredictor::s_bse_scaler_cache_;
std::mutex BsePredictor::s_bse_scaler_mutex_;

// 添加PredictorBase的默认构造函数
PredictorBase::PredictorBase() {
    real_gru_model_ = nullptr;
    gru_ver_1_model_ = nullptr;
    rnn_initialized_ = false;
    hidden_state_ = Eigen::VectorXf::Zero(kHiddenDim);
    last_ob = nullptr;
    scaler_mean_.fill(0.0f);
    scaler_inv_std_.fill(1.0f);
    scaled_features_.fill(0.0f);
    bse_last_ob = nullptr;
    bse_curr_ob = nullptr;
}

SsePredictor::SsePredictor(double threshold, double hm, const std::string& model_type,
                           const std::string& model_path,
                           const std::string& scaler_path) : PredictorBase() {
    threshold_ = threshold;
    history_amount = hm;
    real_gru_model_ = nullptr;
    rnn_initialized_ = false;
    hidden_state_ = Eigen::VectorXf::Zero(kHiddenDim);
    initialize_rnn_model(model_path, scaler_path, model_type);
}

BsePredictor::BsePredictor(double threshold,
                           double history_amount,
                           const std::string& model_type,
                           const std::string& model_path,
                           const std::string& scaler_path) : PredictorBase() {
    threshold_ = threshold;
    this->history_amount = history_amount;
    model_path_ = model_path;
    real_gru_model_ = nullptr;
    gru_ver_1_model_ = nullptr;
    rnn_initialized_ = false;
    hidden_state_ = Eigen::VectorXf::Zero(kHiddenDim);
    predict_count_ = 0;
    bse_scaler_mean_.fill(0.0f);
    bse_scaler_inv_std_.fill(1.0f);
    bse_scaled_features_.fill(0.0f);
    bse_raw_features_.fill(0.0f);
    bse_scaler_loaded_ = false;
    initialize_rnn_model(model_path, scaler_path, model_type);
}

bool BsePredictor::SeedLastSnapshot(const BseSnapshotField& snapshot) {
    if (bse_last_ob != nullptr) {
        return false;
    }
    bse_last_ob = new BseSnapshotField(snapshot);
    return true;
}

void PredictorBase::generate_ob_factors(const MSMarketDataField* cur_ob, OrderBookFactor &factor) {
    const bool latency_enabled = shsz_full_orderbook_latency_enabled();
    ShSzFullOrderBookLatencyStats* stats = latency_enabled ? &shsz_full_orderbook_latency_stats() : 0;
    uint64_t begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;

    factor.weighted_cross_price_rtn = weighted_cross_price_rtn(cur_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_WEIGHTED_CROSS_PRICE_RTN, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.sqrt_trade_ratio = sqrt_trade_ratio(cur_ob,last_ob,history_amount);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_SQRT_TRADE_RATIO, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.spread = spread(cur_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_SPREAD, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.mid_rtn = mid_rtn(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_MID_RTN, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.tick_size = tick_size(cur_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_TICK_SIZE, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.bid_vol_change_ratio = bid_vol_change_ratio(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_BID_VOL_CHANGE_RATIO, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.ask_vol_change_ratio = ask_vol_change_ratio(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_ASK_VOL_CHANGE_RATIO, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.weighted_rtn_lv1 = weighted_rtn_lv1(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_WEIGHTED_RTN_LV1, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.weighted_rtn_lv2 = weighted_rtn_lv2(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_WEIGHTED_RTN_LV2, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.weighted_rtn_lv3 = weighted_rtn_lv3(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_WEIGHTED_RTN_LV3, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.weighted_rtn_lv4 = weighted_rtn_lv4(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_WEIGHTED_RTN_LV4, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.weighted_rtn_lv5 = weighted_rtn_lv5(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_WEIGHTED_RTN_LV5, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.pct_weighted_ask = pct_weighted_ask(cur_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_PCT_WEIGHTED_ASK, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.pct_weighted_bid = pct_weighted_bid(cur_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_PCT_WEIGHTED_BID, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.weighted_ask_rtn = weighted_ask_rtn(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_WEIGHTED_ASK_RTN, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.weighted_bid_rtn = weighted_bid_rtn(cur_ob,last_ob );
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_WEIGHTED_BID_RTN, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.weighted_imabalance_lv5 = weighted_imabalance_lv5(cur_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_WEIGHTED_IMABALANCE_LV5, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.imbalance_lv5 = imbalance_lv5(cur_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_IMBALANCE_LV5, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.pct_turnover = pct_turnover(cur_ob,last_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_PCT_TURNOVER, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.ask_pct_lv1 = ask_pct_lv1(cur_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_ASK_PCT_LV1, shsz_full_orderbook_now_ns() - begin_ns);
    }
    begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    factor.bid_pct_lv1 = bid_pct_lv1(cur_ob);
    if (stats != 0) {
        stats->add_legacy_orderbook_sample(
            SHSZ_LEGACY_OB_BID_PCT_LV1, shsz_full_orderbook_now_ns() - begin_ns);
    }
}

void PredictorBase::generate_ob_factors(const BseSnapshotField *cur_ob, OrderBookFactor &factor) {
    factor.weighted_cross_price_rtn = weighted_cross_price_rtn(cur_ob);
    factor.sqrt_trade_ratio = sqrt_trade_ratio(cur_ob,bse_last_ob,history_amount);
    factor.spread = spread(cur_ob);
    factor.mid_rtn = mid_rtn(cur_ob,bse_last_ob);
    factor.tick_size = tick_size(cur_ob);
    factor.bid_vol_change_ratio = bid_vol_change_ratio(cur_ob,bse_last_ob);
    factor.ask_vol_change_ratio = ask_vol_change_ratio(cur_ob,bse_last_ob);
    factor.weighted_rtn_lv1 = weighted_rtn_lv1(cur_ob,bse_last_ob);
    factor.weighted_rtn_lv2 = weighted_rtn_lv2(cur_ob,bse_last_ob);
    factor.weighted_rtn_lv3 = weighted_rtn_lv3(cur_ob,bse_last_ob);
    factor.weighted_rtn_lv4 = weighted_rtn_lv4(cur_ob,bse_last_ob);
    factor.weighted_rtn_lv5 = weighted_rtn_lv5(cur_ob,bse_last_ob);
    factor.pct_weighted_ask = pct_weighted_ask(cur_ob);
    factor.pct_weighted_bid = pct_weighted_bid(cur_ob);
    factor.weighted_ask_rtn = weighted_ask_rtn(cur_ob,bse_last_ob);
    factor.weighted_bid_rtn = weighted_bid_rtn(cur_ob,bse_last_ob );
    factor.weighted_imabalance_lv5 = weighted_imabalance_lv5(cur_ob);
    factor.imbalance_lv5 = imbalance_lv5(cur_ob);
    factor.pct_turnover = pct_turnover(cur_ob,bse_last_ob);
    factor.ask_pct_lv1 = ask_pct_lv1(cur_ob);
    factor.bid_pct_lv1 = bid_pct_lv1(cur_ob);
}

double PredictorBase::DoPredict(const MSMarketDataField* cur_ob) {
    OrderBookFactor order_book_factor;
    generate_ob_factors(cur_ob, order_book_factor);
    AdvancePredictState(cur_ob);
    return 0.0;
}

void PredictorBase::generate_of_factors(OrderflowFactor& factor) {
    handle_cached_order(factor);
    handle_cached_trade(factor);
    factor.positive_order_flow = factor.positive_order_flow / history_amount * 1e3;
    factor.negative_order_flow = factor.negative_order_flow / history_amount * 1e3;
    factor.market_order_flow = factor.market_order_flow / history_amount * 1e3;
    factor.cancel_buy_flow = factor.cancel_buy_flow / history_amount * 1e3;
    factor.cancel_sell_flow = factor.cancel_sell_flow / history_amount * 1e3;
    factor.positive_trade_flow = factor.positive_trade_flow / history_amount * 1e3;
    factor.negative_trade_flow = factor.negative_trade_flow / history_amount * 1e3;
    factor.positive_trade_num = std::log(1 + factor.positive_trade_num);
    factor.negative_trade_num = std::log(1 + factor.negative_trade_num);
}

void PredictorBase::ProbeFeatureTiming(const MSMarketDataField* cur_ob) {
    (void)cur_ob;
}

void PredictorBase::AdvancePredictState(const MSMarketDataField* cur_ob) {
    if (cur_ob == nullptr) {
        return;
    }
    if (last_ob != nullptr) {
        delete last_ob;
    }
    last_ob = new MSMarketDataField(*cur_ob);
}

bool PredictorBase::MayPredict(const MSMarketDataField *cur_ob) {
    if (cur_ob->MarketTime / 100000 < 930) return false;
    if (last_ob == nullptr) {
        last_ob = new MSMarketDataField(*cur_ob);
        return false;
    }
    if (cur_ob->AskVolume1 == 0 || cur_ob->BidVolume1 == 0)  return false;
    if (cur_ob->Turnover == last_ob->Turnover) return false;
    if (cur_ob->MarketTime == last_ob->MarketTime) return false;
    if (cur_ob->Turnover - last_ob->Turnover > threshold_) {
        return true;
    }
    if (std::abs(MP(cur_ob) - MP(last_ob)) > 1e-6) {
        return true;
    }



    // if (MillSec) return false
    // if (cum_amount_ * downsample_ < threshold_ &&
    //     MID_PRICE(last_ob) == MID_PRICE(cur_ob)
    // ) return false;
    return false;
}


SzePredictor::SzePredictor(double threshold, double hm, const std::string& model_type,
                           const std::string& model_path,
                           const std::string& scaler_path) : PredictorBase() {
    threshold_ = threshold;
    history_amount = hm;
    real_gru_model_ = nullptr;
    rnn_initialized_ = false;
    hidden_state_ = Eigen::VectorXf::Zero(kHiddenDim);
    initialize_rnn_model(model_path, scaler_path, model_type);

}

SzePredictor::~SzePredictor() {
    // 基类析构函数已经处理了模型资源的清理
}

void SzePredictor::handle_trade(const LFL2TradeField* td) {
    td_queue.push(*td);
}

void SzePredictor::handle_order(const LFL2OrderField* od) {
    od_queue.push(*od);
}

void SzePredictor::generate_of_factors(OrderflowFactor& factor) {
    ShSzLegacyOrderflowTimingBreakdown timing;
    process_sze_cached_order_queue(last_ob, price_tick, &od_queue, &factor, &timing);
    process_sze_cached_trade_queue(last_ob, &td_queue, &factor, &timing);
    normalize_sze_orderflow_factors(history_amount, &factor, &timing);
    commit_sze_orderflow_timing(timing);
}

void SzePredictor::ProbeFeatureTiming(const MSMarketDataField* cur_ob) {
    if (cur_ob == nullptr || last_ob == nullptr || !shsz_full_orderbook_latency_enabled()) {
        return;
    }

    OrderBookFactor order_book_factor;
    generate_ob_factors(cur_ob, order_book_factor);

    OrderflowFactor orderflow_factor;
    std::queue<LFL2OrderField> order_queue_copy = od_queue;
    std::queue<LFL2TradeField> trade_queue_copy = td_queue;
    ShSzLegacyOrderflowTimingBreakdown timing;
    process_sze_cached_order_queue(last_ob, price_tick, &order_queue_copy, &orderflow_factor, &timing);
    process_sze_cached_trade_queue(last_ob, &trade_queue_copy, &orderflow_factor, &timing);
    normalize_sze_orderflow_factors(history_amount, &orderflow_factor, &timing);
    commit_sze_orderflow_timing(timing);
}
double SzePredictor::DoPredict(const MSMarketDataField* cur_ob) {
    OrderBookFactor order_book_factor;
    OrderflowFactor orderflow_factor;
    generate_ob_factors(cur_ob, order_book_factor);
    generate_of_factors(orderflow_factor);
    std::vector<float> features = extract_features(orderflow_factor, order_book_factor);

    double prediction = predict(features);

    predict_count_++;
    AdvancePredictState(cur_ob);
    return prediction;
}

void SzePredictor::AdvancePredictState(const MSMarketDataField* cur_ob) {
    if (cur_ob == nullptr) {
        return;
    }
    if (last_ob == nullptr) {
        PredictorBase::AdvancePredictState(cur_ob);
        return;
    }

    OrderBookFactor order_book_factor;
    OrderflowFactor orderflow_factor;
    generate_ob_factors(cur_ob, order_book_factor);
    generate_of_factors(orderflow_factor);
    PredictorBase::AdvancePredictState(cur_ob);
}




void SzePredictor::handle_cached_order(OrderflowFactor &factor) {
    process_sze_cached_order_queue(last_ob, price_tick, &od_queue, &factor, 0);
    od_queue = std::queue<LFL2OrderField>();
}
  
void SzePredictor::handle_cached_trade(OrderflowFactor &factor) {
    process_sze_cached_trade_queue(last_ob, &td_queue, &factor, 0);
    td_queue = std::queue<LFL2TradeField>();
}

// SzePredictor的MayPredict实现
bool SzePredictor::MayPredict(const MSMarketDataField* cur_ob) {
    return PredictorBase::MayPredict(cur_ob);
}

// PredictorBase虚函数实现
void PredictorBase::handle_trade(const LFL2TradeField* td) {
    // 基类默认实现为空
}

void PredictorBase::handle_order(const LFL2OrderField* od) {
    // 基类默认实现为空
}

void PredictorBase::handle_cached_trade(OrderflowFactor& factor) {
    // 基类默认实现为空
}

void PredictorBase::handle_cached_order(OrderflowFactor& factor) {
    // 基类默认实现为空
}

// SsePredictor虚函数实现
void SsePredictor::handle_trade(const LFL2TradeField* td) {
    td_queue.push(*td);
}

void SsePredictor::handle_order(const LFL2OrderField* od) {
    od_queue.push(*od);
}

void SsePredictor::handle_cached_trade(OrderflowFactor& factor) {
    while (!td_queue.empty()) {
        const auto &trade = td_queue.front();
        td_queue.pop();
        if (trade.BidApplSeqNum > trade.OfferApplSeqNum) {
            float bench_price = last_ob->BidPrice1;
            double weight = 1 - std::tanh((bench_price / trade.Price - 1) * 100);
            factor.positive_trade_flow += trade.Volume * trade.Price;
            factor.positive_trade_num += 1;
            factor.positive_order_flow += weight * trade.Price * trade.Volume;
        } else {
            float bench_price = last_ob->AskPrice1;
            double weight = 1 - std::tanh((trade.Price / bench_price - 1) * 100);
            factor.negative_trade_flow += trade.Volume * trade.Price;
            factor.negative_trade_num += 1;
            factor.negative_order_flow += weight * trade.Price * trade.Volume;
        }
    }
    td_queue = std::queue<LFL2TradeField>();
}

void SsePredictor::handle_cached_order(OrderflowFactor& factor) {
    float* positive_order_flow = &(factor.positive_order_flow);
    float* negative_order_flow = &(factor.negative_order_flow);
    float* market_order_flow =  &(factor.market_order_flow);

    while (!od_queue.empty()) {
        const auto &order = od_queue.front();
        od_queue.pop();
        if (order.OrdType[0]=='A') {
            {
                float bench_price = last_ob->BidPrice1;
                float limit_price = last_ob->AskVolume1 == 0 ? bench_price + price_tick : last_ob->AskPrice1;

                if (order.OrderKind[0] == 'B' && order.Price < limit_price - 1e-6) {
                    double weight = 1 - std::tanh((bench_price / order.Price - 1) * 100);
                    *positive_order_flow += order.Volume * order.Price  * weight;
                }
            }
            {
                float bench_price = last_ob->AskPrice1;
                float limit_price = last_ob->BidVolume1 == 0 ? bench_price - price_tick : last_ob->BidPrice1;
                if (order.OrderKind[0] == 'S' && order.Price > limit_price + 1e-6) {
                    double weight = 1 - std::tanh((order.Price / bench_price - 1) * 100);
                    *negative_order_flow += order.Volume * order.Price * weight;
                }
            }
            {
                if (order.OrderKind[0] == 'B') {
                    if (order.OrdType[0] != '2' || order.Price > last_ob->AskPrice1 + 1e-6) {
                        *market_order_flow += order.Volume  * order.Price;
                    }
                } else {
                    if (order.OrdType[0] != '2' || order.Price < last_ob->BidPrice1 - 1e-6) {
                        *market_order_flow -= order.Volume  * order.Price;
                    }
                }
            }
        }
        else  {
            if (order.OrderKind[0] == 'B') {
                factor.cancel_buy_flow += order.Volume * MP(last_ob);
            }else {
                factor.cancel_buy_flow += order.Volume * MP(last_ob);
            }
        }



    }
    od_queue = std::queue<LFL2OrderField>();
}

// SsePredictor的MayPredict和DoPredict实现
bool SsePredictor::MayPredict(const MSMarketDataField* cur_ob) {
    pending = PredictorBase::MayPredict(cur_ob);
    return pending;
}

double SsePredictor::DoPredict(const MSMarketDataField* cur_ob) {
    OrderBookFactor order_book_factor;
    OrderflowFactor orderflow_factor;
    generate_ob_factors(cur_ob, order_book_factor);
    generate_of_factors(orderflow_factor);
    // {
    //     std::ostringstream oss;
    //     oss.setf(std::ios::fixed);
    //     oss << std::setprecision(6);
    //     oss << "[SsePredictor][OrderflowFactor]"
    //         << " positive_order_flow=" << orderflow_factor.positive_order_flow
    //         << " negative_order_flow=" << orderflow_factor.negative_order_flow
    //         << " market_order_flow=" << orderflow_factor.market_order_flow
    //         << " cancel_buy_flow=" << orderflow_factor.cancel_buy_flow
    //         << " cancel_sell_flow=" << orderflow_factor.cancel_sell_flow
    //         << " positive_trade_flow=" << orderflow_factor.positive_trade_flow
    //         << " negative_trade_flow=" << orderflow_factor.negative_trade_flow
    //         << " positive_trade_num=" << orderflow_factor.positive_trade_num
    //         << " negative_trade_num=" << orderflow_factor.negative_trade_num;
    //     std::cout << oss.str() << std::endl;
    // }
    // {
    //     std::ostringstream oss;
    //     oss.setf(std::ios::fixed);
    //     oss << std::setprecision(6);
    //     oss << "[SsePredictor][OrderBookFactor]"
    //         << " weighted_cross_price_rtn=" << order_book_factor.weighted_cross_price_rtn
    //         << " sqrt_trade_ratio=" << order_book_factor.sqrt_trade_ratio
    //         << " spread=" << order_book_factor.spread
    //         << " mid_rtn=" << order_book_factor.mid_rtn
    //         << " tick_size=" << order_book_factor.tick_size
    //         << " bid_vol_change_ratio=" << order_book_factor.bid_vol_change_ratio
    //         << " ask_vol_change_ratio=" << order_book_factor.ask_vol_change_ratio
    //         << " weighted_rtn_lv1=" << order_book_factor.weighted_rtn_lv1
    //         << " weighted_rtn_lv2=" << order_book_factor.weighted_rtn_lv2
    //         << " weighted_rtn_lv3=" << order_book_factor.weighted_rtn_lv3
    //         << " weighted_rtn_lv4=" << order_book_factor.weighted_rtn_lv4
    //         << " weighted_rtn_lv5=" << order_book_factor.weighted_rtn_lv5
    //         << " pct_weighted_ask=" << order_book_factor.pct_weighted_ask
    //         << " pct_weighted_bid=" << order_book_factor.pct_weighted_bid
    //         << " weighted_ask_rtn=" << order_book_factor.weighted_ask_rtn
    //         << " weighted_bid_rtn=" << order_book_factor.weighted_bid_rtn
    //         << " weighted_imabalance_lv5=" << order_book_factor.weighted_imabalance_lv5
    //         << " imbalance_lv5=" << order_book_factor.imbalance_lv5
    //         << " pct_turnover=" << order_book_factor.pct_turnover
    //         << " ask_pct_lv1=" << order_book_factor.ask_pct_lv1
    //         << " bid_pct_lv1=" << order_book_factor.bid_pct_lv1;
    //     std::cout << oss.str() << std::endl;
    // }
    std::vector<float> features = extract_features(orderflow_factor, order_book_factor);

    double prediction = predict(features);

    last_ob = new MSMarketDataField(*cur_ob);
    return prediction;
}




// BsePredictor的MayPredict和DoPredict实现
bool BsePredictor::MayPredict(const BseSnapshotField* cur_ob) {
    if (cur_ob == nullptr) {
        may_predict_reason_ = "null_snapshot";
        return false;
    }
    if (cur_ob->MarketTime / 1e11 < 930) {
        may_predict_reason_ = "pre_open";
        return false;
    }
    if (bse_last_ob == nullptr) {
        bse_last_ob = new BseSnapshotField(*cur_ob);
        may_predict_reason_ = "first_tick";
        return false;
    }
    if (cur_ob->Turnover == bse_last_ob->Turnover) {
        may_predict_reason_ = "turnover_unchanged";
        return false;
    }
    may_predict_reason_ = "ok";
    return true;
}



double BsePredictor::DoPredict(const BseSnapshotField* cur_ob) {
    if (cur_ob == nullptr) return 0.0;

    const uint64_t t0 = bse_now_ns();
    OrderBookFactor order_book_factor;
    generate_ob_factors(cur_ob, order_book_factor);
    const uint64_t t1 = bse_now_ns();
    std::vector<float> features = extract_features(order_book_factor);
    const uint64_t t2 = bse_now_ns();
    double prediction = predict(features);
    const uint64_t t3 = bse_now_ns();

    g_bse_latency_stats.ob_factor_ns += (t1 - t0);
    g_bse_latency_stats.extract_features_ns += (t2 - t1);
    g_bse_latency_stats.model_predict_ns += (t3 - t2);

    if (!std::isfinite(prediction)) {
        static const char* kBseFeatureNames[21] = {
            "weighted_cross_price_rtn", "sqrt_trade_ratio", "spread", "mid_rtn",
            "tick_size", "bid_vol_change_ratio", "ask_vol_change_ratio",
            "weighted_rtn_lv1", "weighted_rtn_lv2", "weighted_rtn_lv3",
            "weighted_rtn_lv4", "weighted_rtn_lv5", "pct_weighted_ask",
            "pct_weighted_bid", "weighted_ask_rtn", "weighted_bid_rtn",
            "weighted_imabalance_lv5", "imbalance_lv5", "pct_turnover",
            "ask_pct_lv1", "bid_pct_lv1"
        };
        std::ostringstream oss;
        oss << std::setprecision(12);
        oss << "[BSE][NaN] InstrumentID=" << cur_ob->InstrumentID
            << " MarketTime=" << std::fixed << std::setprecision(0) << cur_ob->MarketTime
            << " LastPrice=" << std::setprecision(6) << cur_ob->LastPrice
            << " MidPrice=" << cur_ob->MidPrice
            << " Turnover=" << cur_ob->Turnover
            << " Volume=" << cur_ob->Volume
            << " Bid1=" << cur_ob->BidPrice1 << "/" << cur_ob->BidVolume1
            << " Ask1=" << cur_ob->AskPrice1 << "/" << cur_ob->AskVolume1;
        double turnover_diff = bse_last_ob ? (cur_ob->Turnover - bse_last_ob->Turnover) : 0.0;
        double volume_diff = bse_last_ob ? (cur_ob->Volume - bse_last_ob->Volume) : 0.0;
        oss << " TurnoverDiff=" << turnover_diff
            << " VolumeDiff=" << volume_diff;
        if (volume_diff != 0.0) {
            oss << " ATP=" << (turnover_diff / volume_diff);
        }
        oss << " Spread=" << (cur_ob->AskPrice1 - cur_ob->BidPrice1)
            << " MP=" << MP(cur_ob);

        bool has_bad_feature = false;
        for (size_t i = 0; i < features.size(); ++i) {
            if (!std::isfinite(features[i])) {
                if (!has_bad_feature) {
                    oss << " BadFeatures=";
                    has_bad_feature = true;
                }
                oss << kBseFeatureNames[i] << ":" << features[i] << ",";
            }
        }
        if (!has_bad_feature) {
            oss << " BadFeatures=none";
        }
        std::cerr << oss.str() << std::endl;
    }

    if (bse_last_ob == nullptr) {
        void* mem = ::operator new(sizeof(BseSnapshotField));
        bse_last_ob = reinterpret_cast<BseSnapshotField*>(mem);
        new (&bse_last_ob->bse_snapshot) BseSnapshot();
    }
    bse_last_ob->InstrumentID = cur_ob->InstrumentID;
    bse_last_ob->MarketTime = cur_ob->MarketTime;
    bse_last_ob->LastPrice = cur_ob->LastPrice;
    bse_last_ob->MidPrice = cur_ob->MidPrice;
    bse_last_ob->Volume = cur_ob->Volume;
    bse_last_ob->Turnover = cur_ob->Turnover;
    bse_last_ob->BidPrice1 = cur_ob->BidPrice1;
    bse_last_ob->BidPrice2 = cur_ob->BidPrice2;
    bse_last_ob->BidPrice3 = cur_ob->BidPrice3;
    bse_last_ob->BidPrice4 = cur_ob->BidPrice4;
    bse_last_ob->BidPrice5 = cur_ob->BidPrice5;
    bse_last_ob->AskPrice1 = cur_ob->AskPrice1;
    bse_last_ob->AskPrice2 = cur_ob->AskPrice2;
    bse_last_ob->AskPrice3 = cur_ob->AskPrice3;
    bse_last_ob->AskPrice4 = cur_ob->AskPrice4;
    bse_last_ob->AskPrice5 = cur_ob->AskPrice5;
    bse_last_ob->BidVolume1 = cur_ob->BidVolume1;
    bse_last_ob->BidVolume2 = cur_ob->BidVolume2;
    bse_last_ob->BidVolume3 = cur_ob->BidVolume3;
    bse_last_ob->BidVolume4 = cur_ob->BidVolume4;
    bse_last_ob->BidVolume5 = cur_ob->BidVolume5;
    bse_last_ob->AskVolume1 = cur_ob->AskVolume1;
    bse_last_ob->AskVolume2 = cur_ob->AskVolume2;
    bse_last_ob->AskVolume3 = cur_ob->AskVolume3;
    bse_last_ob->AskVolume4 = cur_ob->AskVolume4;
    bse_last_ob->AskVolume5 = cur_ob->AskVolume5;

    predict_count_++;
    return prediction;
}




// SzePredictor的CSV保存函数实现
void SzePredictor::saveToCSV(const MSMarketDataField* cur_ob, const OrderBookFactor& order_book_factor, const OrderflowFactor& orderflow_factor) {
    static bool header_written = false;
    static std::ofstream csv_file("sze_predictor_data.csv", std::ios::app);
    
    // 如果是第一次写入，先写入表头
    if (!header_written) {
        csv_file << "MarketTime,InstrumentID,LastPrice,Volume,Turnover,"
                 << "weight_cross_price,sqrt_trade_ratio,spread,mid_rtn,tick_size,"
                 << "bid_vol_change_ratio,ask_vol_change_ratio,weighted_rtn_lv1,weighted_rtn_lv2,"
                 << "weighted_rtn_lv3,weighted_rtn_lv4,weighted_rtn_lv5,weighted_ask_price,"
                 << "weighted_bid_price,weighted_ask_rtn,weighted_bid_rtn,weighted_imabalance_lv5,"
                 << "imbalance_lv5,pct_turnover,ask_pct_lv1,bid_pct_lv1,"
                 << "positive_order_flow,negative_order_flow,market_order_flow,cancel_buy_flow,"
                 << "cancel_sell_flow,positive_trade_flow,negative_trade_flow" << std::endl;
        header_written = true;
    }
    
    // 写入数据行
    csv_file << cur_ob->MarketTime << "," << cur_ob->InstrumentID << "," << cur_ob->LastPrice << ","
             << cur_ob->Volume << "," << cur_ob->Turnover << ","
             << order_book_factor.weighted_cross_price_rtn << "," << order_book_factor.sqrt_trade_ratio << ","
             << order_book_factor.spread << "," << order_book_factor.mid_rtn << "," << order_book_factor.tick_size << ","
             << order_book_factor.bid_vol_change_ratio << "," << order_book_factor.ask_vol_change_ratio << ","
             << order_book_factor.weighted_rtn_lv1 << "," << order_book_factor.weighted_rtn_lv2 << ","
             << order_book_factor.weighted_rtn_lv3 << "," << order_book_factor.weighted_rtn_lv4 << ","
             << order_book_factor.weighted_rtn_lv5 << "," << order_book_factor.pct_weighted_ask << ","
             << order_book_factor.pct_weighted_bid << "," << order_book_factor.weighted_ask_rtn << ","
             << order_book_factor.weighted_bid_rtn << "," << order_book_factor.weighted_imabalance_lv5 << ","
             << order_book_factor.imbalance_lv5 << "," << order_book_factor.pct_turnover << ","
             << order_book_factor.ask_pct_lv1 << "," << order_book_factor.bid_pct_lv1 << ","
             << orderflow_factor.positive_order_flow << "," << orderflow_factor.negative_order_flow << ","
             << orderflow_factor.market_order_flow << "," << orderflow_factor.cancel_buy_flow << ","
             << orderflow_factor.cancel_sell_flow << "," << orderflow_factor.positive_trade_flow << ","
             << orderflow_factor.negative_trade_flow << std::endl;
    
    csv_file.flush(); // 确保数据立即写入文件
}

// RNN 模型相关方法实现
void PredictorBase::initialize_rnn_model(const std::string& model_path,
                                         const std::string& scaler_path,
                                         const std::string& model_type) {
    model_type_ = model_type;
    use_gru_ver_1_ = (model_type_ == "gru_ver_1");

    if (use_gru_ver_1_) {
        if (!load_scaler(scaler_path)) {
            throw std::runtime_error("Cannot load scaler file: " + scaler_path);
        }
        gru_ver_1_model_ = get_shared_gru_ver_1_model(model_path, kFeatureDim, kHiddenDim);
        rnn_initialized_ = true;
        return;
    }

    real_gru_model_ = get_shared_real_gru_model(model_path, kFeatureDim);
    rnn_initialized_ = true;
}

bool PredictorBase::load_scaler(const std::string& scaler_path) {
    if (scaler_path.empty()) {
        return false;
    }
    std::ifstream file(scaler_path);
    if (!file.is_open()) {
        return false;
    }
    nlohmann::json scaler_json;
    file >> scaler_json;
    auto mean_it = scaler_json.find("mean");
    auto std_it = scaler_json.find("std");
    if (mean_it == scaler_json.end() || std_it == scaler_json.end()) {
        return false;
    }
    const auto& mean = *mean_it;
    const auto& stdv = *std_it;
    if (!mean.is_array() || !stdv.is_array()) {
        return false;
    }
    if (mean.size() != kFeatureDim || stdv.size() != kFeatureDim) {
        return false;
    }
    for (int i = 0; i < kFeatureDim; ++i) {
        float std_val = stdv[i].get<float>();
        scaler_mean_[i] = mean[i].get<float>();
        scaler_inv_std_[i] = std_val != 0.0f ? (1.0f / std_val) : 0.0f;
    }
    scaler_loaded_ = true;
    return true;
}

std::vector<float> PredictorBase::extract_features(const OrderflowFactor& orderflow_factor, const OrderBookFactor& orderbook_factor) {
    std::vector<float> features(kFeatureDim);
    
    // 前9个维度：OrderflowFactor (9个字段)
    features[0] = orderflow_factor.positive_order_flow;
    features[1] = orderflow_factor.negative_order_flow;
    features[2] = orderflow_factor.market_order_flow;
    features[3] = orderflow_factor.cancel_buy_flow;
    features[4] = orderflow_factor.cancel_sell_flow;
    features[5] = orderflow_factor.positive_trade_flow;
    features[6] = orderflow_factor.negative_trade_flow;
    features[7] = orderflow_factor.positive_trade_num;
    features[8] = orderflow_factor.negative_trade_num;
    
    // 后21个维度：OrderBookFactor (21个字段)
    features[9] = orderbook_factor.weighted_cross_price_rtn;
    features[10] = orderbook_factor.sqrt_trade_ratio;
    features[11] = orderbook_factor.spread;
    features[12] = orderbook_factor.mid_rtn;
    features[13] = orderbook_factor.tick_size;
    features[14] = orderbook_factor.bid_vol_change_ratio;
    features[15] = orderbook_factor.ask_vol_change_ratio;
    features[16] = orderbook_factor.weighted_rtn_lv1;
    features[17] = orderbook_factor.weighted_rtn_lv2;
    features[18] = orderbook_factor.weighted_rtn_lv3;
    features[19] = orderbook_factor.weighted_rtn_lv4;
    features[20] = orderbook_factor.weighted_rtn_lv5;
    features[21] = orderbook_factor.pct_weighted_ask;
    features[22] = orderbook_factor.pct_weighted_bid;
    features[23] = orderbook_factor.weighted_ask_rtn;
    features[24] = orderbook_factor.weighted_bid_rtn;
    features[25] = orderbook_factor.weighted_imabalance_lv5;
    features[26] = orderbook_factor.imbalance_lv5;
    features[27] = orderbook_factor.pct_turnover;
    features[28] = orderbook_factor.ask_pct_lv1;
    features[29] = orderbook_factor.bid_pct_lv1;
    
    return features;
}

std::string PredictorBase::get_model_key(const std::string& model_path, int input_dim) {
    std::ostringstream oss;
    oss << model_path << "_REAL_GRU_" << input_dim;
    return oss.str();
}

RealGRU* PredictorBase::get_shared_real_gru_model(const std::string& model_path, int input_dim) {
    std::string key = get_model_key(model_path, input_dim);

    if (s_shared_real_gru_models_.find(key) == s_shared_real_gru_models_.end()) {
        // 第一次加载，创建新实例
        RealGRU* model = new RealGRU(model_path, input_dim);
        s_shared_real_gru_models_[key] = model;
    }

    return s_shared_real_gru_models_[key];
}

std::string PredictorBase::get_gru_ver_1_model_key(const std::string& model_path,
                                                   int input_dim,
                                                   int hidden_dim) {
    std::ostringstream oss;
    oss << model_path << "_GRU_VER_1_" << input_dim << "_" << hidden_dim;
    return oss.str();
}

gru_ver_1* PredictorBase::get_shared_gru_ver_1_model(const std::string& model_path,
                                                     int input_dim,
                                                     int hidden_dim) {
    std::string key = get_gru_ver_1_model_key(model_path, input_dim, hidden_dim);
    if (s_shared_gru_ver_1_models_.find(key) == s_shared_gru_ver_1_models_.end()) {
        gru_ver_1* model = new gru_ver_1(model_path, input_dim, hidden_dim);
        s_shared_gru_ver_1_models_[key] = model;
    }
    return s_shared_gru_ver_1_models_[key];
}

float PredictorBase::predict(const std::vector<float>& features) {
    if (!rnn_initialized_) {
        return 0.0f;
    }
    if (use_gru_ver_1_) {
        if (gru_ver_1_model_ == nullptr || !scaler_loaded_) {
            return 0.0f;
        }
        const float* src = features.data();
        for (int i = 0; i < kFeatureDim; ++i) {
            scaled_features_[i] = (src[i] - scaler_mean_[i]) * scaler_inv_std_[i];
        }
        Eigen::Map<const Eigen::VectorXf> input_vec(scaled_features_.data(), kFeatureDim);
        Eigen::VectorXf output = gru_ver_1_model_->forward(input_vec, hidden_state_);
        return output(0);
    }
    if (real_gru_model_ == nullptr) {
        return 0.0f;
    }
    Eigen::Map<const Eigen::VectorXf> input_vec(features.data(), static_cast<int>(features.size()));
    Eigen::VectorXf output = real_gru_model_->forward(input_vec, hidden_state_);
    return output(0);
}

float BsePredictor::predict(const std::vector<float>& features) {
    if (!rnn_initialized_) {
        return 0.0f;
    }
    if (use_gru_ver_1_) {
        if (gru_ver_1_model_ == nullptr || !bse_scaler_loaded_) {
            return 0.0f;
        }
        if (features.size() < static_cast<size_t>(kBseFeatureDim)) {
            return 0.0f;
        }
        const float* src = features.data();
        for (int i = 0; i < kBseFeatureDim; ++i) {
            bse_raw_features_[i] = src[i];
            debug_snapshot_.raw_features[i] = src[i];
        }
        const Eigen::VectorXf hidden_in = hidden_state_;
        for (int i = 0; i < kHiddenDim; ++i) {
            debug_snapshot_.hidden_in[i] = hidden_in(i);
        }
        for (int i = 0; i < kBseFeatureDim; ++i) {
            bse_scaled_features_[i] = (src[i] - bse_scaler_mean_[i]) * bse_scaler_inv_std_[i];
            debug_snapshot_.scaled_features[i] = bse_scaled_features_[i];
        }
        Eigen::Map<const Eigen::VectorXf> input_vec(bse_scaled_features_.data(), kBseFeatureDim);
        Eigen::VectorXf output = gru_ver_1_model_->forward(input_vec, hidden_state_);
        for (int i = 0; i < kHiddenDim; ++i) {
            debug_snapshot_.hidden_out[i] = hidden_state_(i);
        }
        debug_snapshot_.prediction = output(0);
        debug_snapshot_.valid = true;
        return output(0);
    }
    return PredictorBase::predict(features);
}

bool BsePredictor::load_bse_scaler(const std::string& scaler_path) {
    BseScalerCacheEntry entry;
    if (!get_shared_bse_scaler(scaler_path, &entry)) {
        return false;
    }
    bse_scaler_mean_ = entry.mean;
    bse_scaler_inv_std_ = entry.inv_std;
    bse_scaler_loaded_ = true;
    return true;
}

bool BsePredictor::get_shared_bse_scaler(const std::string& scaler_path,
                                         BseScalerCacheEntry* out) {
    if (scaler_path.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_bse_scaler_mutex_);
        auto it = s_bse_scaler_cache_.find(scaler_path);
        if (it != s_bse_scaler_cache_.end()) {
            if (out != nullptr) {
                *out = it->second;
            }
            return true;
        }
    }

    std::ifstream file(scaler_path);
    if (!file.is_open()) {
        return false;
    }
    nlohmann::json scaler_json;
    file >> scaler_json;
    auto mean_it = scaler_json.find("mean");
    auto std_it = scaler_json.find("std");
    if (mean_it == scaler_json.end() || std_it == scaler_json.end()) {
        return false;
    }
    const auto& mean = *mean_it;
    const auto& stdv = *std_it;
    if (!mean.is_array() || !stdv.is_array()) {
        return false;
    }
    if (mean.size() != kBseFeatureDim || stdv.size() != kBseFeatureDim) {
        return false;
    }
    BseScalerCacheEntry parsed;
    for (int i = 0; i < kBseFeatureDim; ++i) {
        float std_val = stdv[i].get<float>();
        parsed.mean[i] = mean[i].get<float>();
        parsed.inv_std[i] = std_val != 0.0f ? (1.0f / std_val) : 0.0f;
    }

    {
        std::lock_guard<std::mutex> lock(s_bse_scaler_mutex_);
        auto result = s_bse_scaler_cache_.emplace(scaler_path, parsed);
        if (out != nullptr) {
            *out = result.first->second;
        }
    }
    return true;
}

void BsePredictor::initialize_rnn_model(const std::string& model_path,
                                        const std::string& scaler_path,
                                        const std::string& model_type) {
    model_type_ = model_type;
    use_gru_ver_1_ = (model_type_ == "gru_ver_1");

    if (use_gru_ver_1_) {
        if (!load_bse_scaler(scaler_path)) {
            throw std::runtime_error("Cannot load scaler file: " + scaler_path);
        }
        gru_ver_1_model_ = get_shared_gru_ver_1_model(model_path, kBseFeatureDim, kHiddenDim);
        rnn_initialized_ = true;
        return;
    }

    real_gru_model_ = get_shared_real_gru_model(model_path, kBseFeatureDim);
    rnn_initialized_ = true;
}


std::vector<float> BsePredictor::extract_features(const OrderBookFactor& orderbook_factor) {
    std::vector<float> features(21);
    
    // 21个OrderBookFactor特征（没有OrderflowFactor）
    features[0] = orderbook_factor.weighted_cross_price_rtn;
    features[1] = orderbook_factor.sqrt_trade_ratio;
    features[2] = orderbook_factor.spread;
    features[3] = orderbook_factor.mid_rtn;
    features[4] = orderbook_factor.tick_size;
    features[5] = orderbook_factor.bid_vol_change_ratio;
    features[6] = orderbook_factor.ask_vol_change_ratio;
    features[7] = orderbook_factor.weighted_rtn_lv1;
    features[8] = orderbook_factor.weighted_rtn_lv2;
    features[9] = orderbook_factor.weighted_rtn_lv3;
    features[10] = orderbook_factor.weighted_rtn_lv4;
    features[11] = orderbook_factor.weighted_rtn_lv5;
    features[12] = orderbook_factor.pct_weighted_ask;
    features[13] = orderbook_factor.pct_weighted_bid;
    features[14] = orderbook_factor.weighted_ask_rtn;
    features[15] = orderbook_factor.weighted_bid_rtn;
    features[16] = orderbook_factor.weighted_imabalance_lv5;
    features[17] = orderbook_factor.imbalance_lv5;
    features[18] = orderbook_factor.pct_turnover;
    features[19] = orderbook_factor.ask_pct_lv1;
    features[20] = orderbook_factor.bid_pct_lv1;
    
    return features;
}
