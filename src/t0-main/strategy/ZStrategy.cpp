//
// Created by Administrator on 25-9-14.
//


#include "ZStrategy.h"
#include "StrategyBase.h"
#include "sze_position_risk.h"
#include "../common.h"
#include "../total_header.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include "../util.h"

namespace {
constexpr int kRiskErrOverOrderCnt = 2010;
constexpr int kRiskErrOverPerSecCnt = 2011;
constexpr int kRiskErrShortOfCancelNum = 3005;
constexpr int kRiskErrNoClosePosition = 3007;
constexpr int kRiskErrNoAvailMoney = 3008;
constexpr long long kRiskCooldownNs = 1000LL * 1000LL * 1000LL;

int NormalizeRiskCode(int request_id) {
    return request_id < 0 ? -request_id : request_id;
}

long OrderStateKey(int request_id, long order_ref) {
    if (request_id < 0) {
        return order_ref;
    }
    return (static_cast<long>(request_id) << 32) ^ (order_ref & 0xffffffffL);
}

const char* RiskCodeText(int error_id) {
    switch (error_id) {
        case kRiskErrOverOrderCnt:
            return "OVER_ORDERCNT";
        case kRiskErrOverPerSecCnt:
            return "OVER_PER_SEC_CNT";
        case kRiskErrShortOfCancelNum:
            return "SHORT_OF_CANCELNUM";
        case kRiskErrNoClosePosition:
            return "NO_CLOSE_POSITION";
        case kRiskErrNoAvailMoney:
            return "NO_AVAIL_MONEY";
        default:
            return "UNHANDLED_RISK";
    }
}
}

ZStrategy::ZStrategy(const std::string &InstrumentID,
    const InsParams &ins_params,
    json &config,
    WCStrategyUtilPtr other_util,
    KfLogPtr other_logger):j_config(config),mTradeInstrument(InstrumentID), last_ob_ptr(nullptr) {
    util=other_util;
    logger=other_logger;
    context.last_ob = nullptr;
    context.curr_ob = nullptr;
    context.name = nullptr;
    if(SHPrefix.find(mTradeInstrument.substr(0,2))!=SHPrefix.end()){
        ExchangeID="SSE";
    }else if(SZPrefix.find(mTradeInstrument.substr(0,2))!=SZPrefix.end()){
        ExchangeID="SZE";
    }else{
        ExchangeID="xxx";
    }

    if (config.find("td_source_index") != config.end() &&
        config["td_source_index"].is_array() &&
        !config["td_source_index"].empty()) {
        td_source_ = static_cast<short>(config["td_source_index"][0].get<int>());
    }
    if (config.find("sze_order_routing") != config.end() &&
        config["sze_order_routing"].is_object()) {
        const json& routing = config["sze_order_routing"];
        if (routing.find("enabled") != routing.end() && routing["enabled"].is_boolean()) {
            routing_enabled_ = routing["enabled"].get<bool>();
        }
        if (routing.find("mode") != routing.end() && routing["mode"].is_string()) {
            virtual_routing_ = routing["mode"].get<std::string>() == "virtual";
        }
        recovery_routing_ = routing.value("input_mode", std::string()) == "recovery_handoff";
        max_position_ = routing.value("max_position", 0);
    }
    if (config.find("sze_startup_warmup_signals") != config.end() &&
        config["sze_startup_warmup_signals"].is_number_integer()) {
        startup_warmup_signal_count_ =
            config["sze_startup_warmup_signals"].get<int>();
    }
    if (startup_warmup_signal_count_ < 0) {
        startup_warmup_signal_count_ = 0;
    }
    KF_LOG_INFO(logger, "[SZEWarmup] instrument=" << mTradeInstrument
        << " prediction_only_samples=" << startup_warmup_signal_count_);

    if (config.find("sze_test_order") != config.end() &&
        config["sze_test_order"].is_object()) {
        const json& test = config["sze_test_order"];
        if (test.find("enabled") != test.end() && test["enabled"].is_boolean()) {
            test_order_.enabled = test["enabled"].get<bool>();
        }
        if (test.find("instrument") != test.end() && test["instrument"].is_string()) {
            test_order_.instrument = test["instrument"].get<std::string>();
        }
        if (test.find("side") != test.end() && test["side"].is_string()) {
            test_order_.direction = test["side"].get<std::string>() == "sell" ? SELL : BUY;
        }
        if (test.find("price") != test.end() && test["price"].is_number()) {
            test_order_.price = test["price"].get<double>();
        }
        if (test.find("volume") != test.end() && test["volume"].is_number_integer()) {
            test_order_.volume = test["volume"].get<int>();
        }
        if (test.find("trigger_after_signals") != test.end() &&
            test["trigger_after_signals"].is_number_integer()) {
            test_order_.trigger_after_signals = test["trigger_after_signals"].get<int>();
        }
        if (test.find("cancel_delay_ms") != test.end() &&
            test["cancel_delay_ms"].is_number_integer()) {
            test_order_.cancel_delay_ms = test["cancel_delay_ms"].get<int>();
        }
        if (test_order_.volume <= 0) {
            test_order_.enabled = false;
        }
        if (test_order_.trigger_after_signals < 1) {
            test_order_.trigger_after_signals = 1;
        }
        if (test_order_.cancel_delay_ms < 0) {
            test_order_.cancel_delay_ms = 0;
        }
    }

    
    if (config.find("global_params") != config.end()) {
        auto& global_params = config["global_params"];
        if (global_params.find("offset") != global_params.end()) {
            g_params.offset_base_line = global_params["offset"].get<double>();
        }
        if (global_params.find("quote_offset") != global_params.end()) {
            g_params.offset = global_params["quote_offset"].get<double>();
        }
        if (global_params.find("global_bias_factor") != global_params.end()) {
            g_params.global_bias_factor = global_params["global_bias_factor"].get<double>();
        } else if (global_params.find("bias_factor") != global_params.end()) {
            g_params.global_bias_factor = global_params["bias_factor"].get<double>();
        }
        global_bias_factor_base_line_ = g_params.global_bias_factor;
        if (global_params.find("position_limit") != global_params.end()) {
            g_params.position_limit = global_params["position_limit"].get<double>();
            g_params.position_limit_base_line = g_params.position_limit;
        }
        if (global_params.find("position_penalty_factor") != global_params.end()) {
            g_params.position_penalty_factor = global_params["position_penalty_factor"].get<double>();
        }
        if (global_params.find("position_base_line") != global_params.end()) {
            g_params.position_base_line = global_params["position_base_line"].get<double>();
        }
        if (global_params.find("can_quote") != global_params.end()) {
            g_params.can_quote = global_params["can_quote"].get<int>();
        }
    }
    
    i_params.static_position = ins_params.static_position;
    i_params.last_position = ins_params.last_position;
    i_params.shortable = ins_params.static_position + ins_params.last_position;
    context.pi = ins_params.last_position;
    if (config.find("ins_params") != config.end() && config["ins_params"].is_object()) {
        const std::string symbol_key = mTradeInstrument + ".SZ";
        json::const_iterator item = config["ins_params"].find(symbol_key);
        if (item == config["ins_params"].end()) {
            item = config["ins_params"].find(mTradeInstrument);
        }
        if (item != config["ins_params"].end() && item->is_object()) {
            const json& values = *item;
            if (values.find("max_order_size") != values.end() &&
                values["max_order_size"].is_number()) {
                i_params.max_order_size = values["max_order_size"].get<double>();
            }
            if (values.find("min_order_size") != values.end() &&
                values["min_order_size"].is_number()) {
                i_params.min_order_size = values["min_order_size"].get<double>();
            }
            if (values.find("vol_unit") != values.end() &&
                values["vol_unit"].is_number_integer()) {
                const int lot = values["vol_unit"].get<int>();
                if (lot > 0) {
                    i_params.vol_unit = lot;
                }
            }
        }
    }
}

void ZStrategy::sync_startup_position(int32_t total_position, int32_t available_position) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const sze_position_risk::StartupPosition startup =
        sze_position_risk::NormalizeStartupPosition(
            i_params.static_position, total_position, available_position);
    context.pi = startup.delta_from_static;
    i_params.last_position = startup.delta_from_static;
    i_params.shortable = startup.available;
    KF_LOG_INFO(logger, "[SZEPosSync] InstrumentID=" << mTradeInstrument
        << ", total_position=" << startup.total
        << ", available_position=" << startup.available
        << ", static_position=" << i_params.static_position
        << ", pi=" << context.pi
        << ", last_position=" << i_params.last_position
        << ", shortable=" << i_params.shortable);
}

std::int32_t ZStrategy::getPositionLimit() {
    return  static_cast<int32_t>(g_params.position_limit * i_params.static_position);
}

std::int32_t ZStrategy::getRemainingShortable() {
    // T0当日剩余可卖： shortable - 剩余未执行的卖出量
    // 如果是调入，则剩余未执行的卖出量为0
    return i_params.shortable;
}

bool ZStrategy::can_send_order(DirectionEnum direction, long long now) const {
    if (now < order_reject_throttle_until_nano_) {
        return false;
    }
    if (direction == BUY && now < buy_block_until_nano_) {
        return false;
    }
    if (direction == SELL && now < sell_block_until_nano_) {
        return false;
    }
    return true;
}

void ZStrategy::on_order_reject(int request_id, const RT_Order& order) {
    const int error_id = NormalizeRiskCode(request_id);
    const long long now = util ? util->get_nano() : 0;
    if (error_id == kRiskErrOverOrderCnt || error_id == kRiskErrOverPerSecCnt) {
        order_reject_throttle_until_nano_ = std::max(order_reject_throttle_until_nano_, now + kRiskCooldownNs);
    } else if (error_id == kRiskErrNoClosePosition) {
        sell_block_until_nano_ = std::max(sell_block_until_nano_, now + kRiskCooldownNs);
    } else if (error_id == kRiskErrNoAvailMoney) {
        buy_block_until_nano_ = std::max(buy_block_until_nano_, now + kRiskCooldownNs);
    }

    if (error_id == kRiskErrOverOrderCnt || error_id == kRiskErrOverPerSecCnt ||
        error_id == kRiskErrShortOfCancelNum || error_id == kRiskErrNoClosePosition ||
        error_id == kRiskErrNoAvailMoney) {
        KF_LOG_INFO(logger, "[RiskReject] InstrumentID=" << mTradeInstrument
            << ", request_id=" << request_id
            << ", error_id=" << error_id
            << ", reason=" << RiskCodeText(error_id)
            << ", side=" << (order.Direction == BUY ? "Buy" : "Sell")
            << ", volume=" << order.Volume
            << ", price=" << order.Price);
    }
}

int ZStrategy::insertOrder(RT_Order order, int cancel_delay_ms) {
    if (!routing_enabled_) {
        KF_LOG_ERROR(logger, "[SZEOrderBlocked] InstrumentID=" << mTradeInstrument
            << ", reason=routing_disabled");
        return -1;
    }
    if (virtual_routing_) {
        KF_LOG_INFO(logger, "[SZEVirtualOrderIntent] InstrumentID=" << mTradeInstrument
            << ", Side=" << (order.Direction == BUY ? "Buy" : "Sell")
            << ", Volume=" << order.Volume
            << ", Price=" << order.Price
            << ", Exchange=" << ExchangeID
            << ", td_source=" << td_source_);
        return -1;
    }
    if (recovery_routing_) {
        // T0 sells may close both the static position and shares bought
        // earlier in the session. Buy size is additionally capped by the
        // configured position limit; sell size is governed by shortable.
        if (order.Volume <= 0 ||
            (order.Direction == BUY &&
             (max_position_ <= 0 || order.Volume > max_position_))) {
            KF_LOG_ERROR(logger, "[SZEOrderBlocked] InstrumentID=" << mTradeInstrument
                << ", reason=invalid_volume_or_buy_position_limit"
                << ", side=" << (order.Direction == BUY ? "Buy" : "Sell")
                << ", volume=" << order.Volume
                << ", max_buy_position=" << max_position_);
            return -1;
        }
    }
    char direction = '0';
    char offsetFlag = '0';
    if (order.Direction == BUY) {

        direction = LF_CHAR_Buy;
        offsetFlag = LF_CHAR_Open;
    }
    if (order.Direction == SELL) {

        direction = LF_CHAR_Sell;
        offsetFlag = LF_CHAR_Close;
    }
    int request_id =-1;
    if (order.Type == FAK) {
#ifdef T0_VIRTUAL_TRADING
        KF_LOG_INFO(logger, "[VirtualOrderIntent] InstrumentID=" << mTradeInstrument
            << ", Side=" << (order.Direction == BUY ? "Buy" : "Sell")
            << ", Volume=" << order.Volume
            << ", Price=" << order.Price
            << ", Type=FAK"
            << ", Exchange=" << ExchangeID
            << ", td_source=" << td_source_
            << ", action=skip_insert_limit_order");
        return -1;
#else
        request_id = util->insert_limit_order(td_source_,mTradeInstrument,
            ExchangeID,order.Price,order.Volume,direction,offsetFlag);
        // KF_LOG_INFO(logger, "[InsertOrder] InstrumentID: " << mTradeInstrument
        //             << ", RequestID: " << request_id
        //             << ", Direction: " << (order.Direction == BUY ? "Buy" : "Sell")
        //             << ", Price: " << order.Price
        //             << ", Volume: " << order.Volume
        //             << ", Type: FAK"
        //             << ", vl_pos: " << context.vl_pos
        //             << ", vs_pos: " << context.vs_pos);
        if (request_id >= 0) {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                cancel_pending_request_ids_.insert(request_id);
            }
            delay_cancel_order(request_id, std::max(0, cancel_delay_ms));
        } else {
            on_order_reject(request_id, order);
        }
#endif
    }
    return request_id;
}

void ZStrategy::maybe_send_test_order() {
    if (!test_order_.enabled || test_order_sent_ || virtual_routing_ ||
        !routing_enabled_ || context.curr_ob == nullptr) {
        return;
    }
    if (!test_order_.instrument.empty() && test_order_.instrument != mTradeInstrument) {
        return;
    }
    const double book_price = test_order_.direction == BUY
        ? context.curr_ob->AskPrice1 : context.curr_ob->BidPrice1;
    const double price = test_order_.price > 0.0 ? test_order_.price : book_price;
    const int lot = i_params.vol_unit > 0 ? i_params.vol_unit : vol_unit;
    const int volume = std::max(lot, test_order_.volume / lot * lot);
    if (price <= 0.0 || volume <= 0) {
        KF_LOG_ERROR(logger, "[SZTestOrder] skipped invalid market snapshot price=" << price
            << " volume=" << volume);
        test_order_sent_ = true;
        return;
    }

    test_order_sent_ = true;
    RT_Order order;
    order.Price = price;
    order.Volume = volume;
    order.Direction = test_order_.direction;
    order.Type = FAK;
    const int request_id = insertOrder(order, test_order_.cancel_delay_ms);
    KF_LOG_INFO(logger, "[SZTestOrder] submitted instrument=" << mTradeInstrument
        << " side=" << (order.Direction == BUY ? "buy" : "sell")
        << " price=" << order.Price << " volume=" << order.Volume
        << " request_id=" << request_id
        << " cancel_delay_ms=" << test_order_.cancel_delay_ms);
}



void ZStrategy::setOffset() {
    double market_time_minutes = -1;
    if (context.curr_ob != nullptr) {
        market_time_minutes = context.curr_ob->MarketTime / 100000.0;
    }

    double offset_multiplier = 1.0;
    if (market_time_minutes >= 0) {
        if (market_time_minutes < 931) {
            offset_multiplier = 5.0;
        } else if (market_time_minutes < 940) {
            const double ramp_progress = (market_time_minutes - 931.0) / 9.0;
            offset_multiplier = 5.0 - 4.0 * std::max(0.0, std::min(1.0, ramp_progress));
        } else if (market_time_minutes >= 1430) {
            offset_multiplier = 0.8;
        }
    }
    g_params.offset = g_params.offset_base_line * pred_unit * offset_multiplier;

    g_params.global_bias_factor = global_bias_factor_base_line_;

    // Keep the full one-unit position limit throughout the trading day.
    // Session/order gates remain responsible for preventing pre-open trading;
    // this value must not shrink after 14:30 and create non-lot-sized caps.
    g_params.position_limit = 1.0;
}

void ZStrategy::cancel_order(int request_id) {
    if (request_id < 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto pending = cancel_pending_request_ids_.find(request_id);
        if (pending == cancel_pending_request_ids_.end()) {
            KF_LOG_INFO(logger, "[CancelSkip] InstrumentID=" << mTradeInstrument
                << ", request_id=" << request_id
                << ", reason=already_terminal_or_cancelled");
            return;
        }
        // Consume the one-shot timer before calling the gateway. This also
        // prevents duplicate callbacks from issuing duplicate cancellations.
        cancel_pending_request_ids_.erase(pending);
    }
    if (!routing_enabled_ || virtual_routing_) {
        KF_LOG_INFO(logger, "[SZEVirtualCancelIntent] InstrumentID=" << mTradeInstrument
            << ", request_id=" << request_id
            << ", td_source=" << td_source_);
        return;
    }
#ifdef T0_VIRTUAL_TRADING
    KF_LOG_INFO(logger, "[VirtualCancelIntent] InstrumentID=" << mTradeInstrument
        << ", request_id=" << request_id
        << ", td_source=" << td_source_
        << ", action=skip_cancel_order");
    return;
#else
    const int cancel_ret = util->cancel_order(td_source_, request_id);
    if (cancel_ret < 0) {
        const int error_id = NormalizeRiskCode(cancel_ret);
        if (error_id == kRiskErrShortOfCancelNum) {
            KF_LOG_INFO(logger, "[RiskCancel] InstrumentID=" << mTradeInstrument
                << ", request_id=" << request_id
                << ", error_id=" << error_id
                << ", reason=" << RiskCodeText(error_id));
        } else {
            KF_LOG_INFO(logger, "[RiskCancel] InstrumentID=" << mTradeInstrument
                << ", request_id=" << request_id
                << ", error_id=" << error_id
                << ", reason=" << RiskCodeText(error_id));
        }
    }
#endif
}

void ZStrategy::delay_cancel_order(int request_id,int delay_ms) {
    if (request_id < 0) {
        return;
    }
    BLCallback callback = std::bind(&ZStrategy::cancel_order,this,request_id);
    const long long now_nano = util ? util->get_nano() : 0;
    util->insert_callback(now_nano + delay_ms * 1000LL * 1000LL, callback);
}

double ZStrategy::getCurPosition() {
    if (context.curr_ob == nullptr) {
        return 0.0;
    }
    return (context.pi + context.vl_pos + context.vs_pos) * context.curr_ob->LastPrice;
}

void ZStrategy::setGlobalPredAdjFactor(double factor) {
    g_params.global_pred_adj_factor = factor;
}

void ZStrategy::set_latency_trace(const StrategyLatencyTrace& trace) {
    latency_trace_ = trace;
}

void ZStrategy::calcTheo(double prediction) {
    setOffset();
    theo_.bias = sze_position_risk::Bias(
        getCurPosition(), g_params.position_base_line, g_params.global_bias_factor);
    theo_.unitbias = sze_position_risk::UnitBias(
        g_params.offset, g_params.global_bias_factor,
        context.curr_ob->LastPrice, g_params.position_base_line);
    theo_.theo0 = MP(context.curr_ob) * (1 + prediction * pred_unit);

    theo_.b_offset = (1 - theo_.bias * g_params.offset - g_params.offset - g_params.global_pred_adj_factor);
    theo_.s_offset = (1 - theo_.bias * g_params.offset + g_params.offset - g_params.global_pred_adj_factor);
    theo_.hit_buy_theo = theo_.b_offset * theo_.theo0;
    theo_.hit_sell_theo = theo_.s_offset * theo_.theo0;
    current_prediction_ = prediction;

}


void ZStrategy::handleT0() {
    cancelBuy();
    cancelSell();
    const long long now = util ? util->get_nano() : 0;
    if (context.CanBuy() && can_send_order(BUY, now)) {
        hitBuy();
    }

    if (context.CanSell() && can_send_order(SELL, now)) {
        hitSell();
    }
}

void ZStrategy::hitBuy() {
    int32_t hit_buy_qty = 0;
    auto cur_ob = &(context.last_ob);
    double buy_margin = theo_.hit_buy_theo / context.curr_ob->AskPrice1 - 1;
    if (buy_margin > 0) {
        hit_buy_qty =
            std::min(static_cast<std::int32_t>(buy_margin / theo_.unitbias), static_cast<std::int32_t>(context.curr_ob->AskVolume1));
        hit_buy_qty = std::min(hit_buy_qty, static_cast<int32_t>(i_params.max_order_size / context.curr_ob->AskPrice1));
        const int32_t lot = i_params.vol_unit > 0 ? i_params.vol_unit : vol_unit;
        hit_buy_qty = std::min(hit_buy_qty / lot * lot,maxCanBuy());
        if (hit_buy_qty < lot) {
            // KF_LOG_INFO(logger, "[HitBuy] InstrumentID: " << mTradeInstrument
            //             << ", Qty too small: " << hit_buy_qty << ", skip");
            return;
        }

        RT_Order order;
        order.Volume = hit_buy_qty;
        order.Price = context.curr_ob->AskPrice1;
        order.Direction = BUY;
        order.Type=FAK;
        int request_id = insertOrder(order);
        if (request_id >= 0) {
            context.vl_pos += order.Volume;
        }
        double cur_position = getCurPosition();
        KF_LOG_INFO(logger, "[HitBuy] InstrumentID: " << mTradeInstrument
            << ", Prediction: " << current_prediction_
            << ", Margin: " << buy_margin
            << ", Qty: " << hit_buy_qty
            << ", Price: " << order.Price
            << ", AskPrice1: " <<  context.curr_ob->AskPrice1
            << ", BidPrice1: " << context.curr_ob->BidPrice1
            << ", AskVolume1: " << context.curr_ob->AskVolume1
            << ", BidVolume1: " << context.curr_ob->BidVolume1
            << ", maxCanBuy: " << maxCanBuy()
            << ", maxCanSell: " << maxCanSell()
            << ", CumBuy: " << context.cum_buy
            << ", CumSell: " << context.cum_sell
            << ", shortable: " << getRemainingShortable()
            << ", RequestID: " << request_id
            << ", latency_market_receive_mono_ns: " << latency_trace_.market_receive_mono_ns
            << ", market_receive_realtime_ns: " << context.market_receive_realtime_ns
            << ", latency_consumer_begin_mono_ns: " << latency_trace_.consumer_begin_mono_ns
            << ", latency_book_done_mono_ns: " << latency_trace_.book_done_mono_ns
            << ", latency_sample_done_mono_ns: " << latency_trace_.sample_done_mono_ns
            << ", latency_factor_done_mono_ns: " << latency_trace_.factor_done_mono_ns
            << ", latency_model_done_mono_ns: " << latency_trace_.model_done_mono_ns
            << ", latency_prediction_done_mono_ns: " << latency_trace_.prediction_done_mono_ns
            << ", latency_signal_decision_mono_ns: " << latency_trace_.signal_decision_mono_ns
            << ", HitBuyTheo: " << theo_.hit_buy_theo
            << ", vl_pos: " << context.vl_pos
            << ", vs_pos: " << context.vs_pos
            << ", CurrentPosition: " << cur_position);
    }
}


void ZStrategy::hitSell() {
    std::int32_t hit_sell_qty = 0;
    auto cur_ob = &(context.last_ob);
    double sell_margin =  context.curr_ob->BidPrice1 / theo_.hit_sell_theo - 1;
    if (sell_margin > 0) {
        hit_sell_qty =
            std::min(static_cast<std::int32_t>(sell_margin / theo_.unitbias), static_cast<std::int32_t>( context.curr_ob->BidVolume1));
        hit_sell_qty =
            std::min(hit_sell_qty, static_cast<int32_t>(i_params.max_order_size /  context.curr_ob->BidPrice1));
        const int32_t lot = i_params.vol_unit > 0 ? i_params.vol_unit : vol_unit;
        hit_sell_qty = std::min(hit_sell_qty / lot * lot,maxCanSell());
        if (hit_sell_qty < lot) {
            // KF_LOG_INFO(logger, "[HitSell] InstrumentID: " << mTradeInstrument
            //             << ", Qty too small: " << hit_sell_qty << ", skip");
            return;
        }
        RT_Order order;
        order.Volume = hit_sell_qty;
        order.Price = context.curr_ob->BidPrice1;
        order.Direction = SELL;
        order.Type=FAK;

        int request_id = insertOrder(order);
        if (request_id >= 0) {
            context.vs_pos += order.Volume;
        }
        double cur_position = getCurPosition();
        KF_LOG_INFO(logger, "[HitSell] InstrumentID: " << mTradeInstrument
            << ", Prediction: " << current_prediction_
            << ", Margin: " << sell_margin
            << ", Qty: " << hit_sell_qty
            << ", Price: " << order.Price
            << ", AskPrice1: " << context.curr_ob->AskPrice1
            << ", BidPrice1: " << context.curr_ob->BidPrice1
            << ", AskVolume1: " << context.curr_ob->AskVolume1
            << ", BidVolume1: " << context.curr_ob->BidVolume1
            << ", maxCanBuy: " << maxCanBuy()
            << ", maxCanSell: " << maxCanSell()
            << ", CumBuy: " << context.cum_buy
            << ", CumSell: " << context.cum_sell
            << ", shortable: " << getRemainingShortable()
            << ", RequestID: " << request_id
            << ", latency_market_receive_mono_ns: " << latency_trace_.market_receive_mono_ns
            << ", market_receive_realtime_ns: " << context.market_receive_realtime_ns
            << ", latency_consumer_begin_mono_ns: " << latency_trace_.consumer_begin_mono_ns
            << ", latency_book_done_mono_ns: " << latency_trace_.book_done_mono_ns
            << ", latency_sample_done_mono_ns: " << latency_trace_.sample_done_mono_ns
            << ", latency_factor_done_mono_ns: " << latency_trace_.factor_done_mono_ns
            << ", latency_model_done_mono_ns: " << latency_trace_.model_done_mono_ns
            << ", latency_prediction_done_mono_ns: " << latency_trace_.prediction_done_mono_ns
            << ", latency_signal_decision_mono_ns: " << latency_trace_.signal_decision_mono_ns
            << ", HitSellTheo: " << theo_.hit_sell_theo
            << ", vl_pos: " << context.vl_pos
            << ", vs_pos: " << context.vs_pos
            << ", CurrentPosition: " << cur_position);
    }
}

int32_t ZStrategy::maxCanBuy() {
    int32_t qty = sze_position_risk::MaxCanBuy(
        getRemainingShortable(), getPositionLimit(), context.pi, context.vl_pos);
    if (context.curr_ob != nullptr && context.curr_ob->LastPrice > 0.0) {
        qty = std::min(qty, static_cast<int32_t>(i_params.max_order_size / context.curr_ob->LastPrice));
    } else {
        qty = 0;
    }
    return std::max<int32_t>(qty, 0);
}

int32_t ZStrategy::maxCanSell() {
    return sze_position_risk::MaxCanSell(
        getRemainingShortable(), getPositionLimit(), i_params.static_position,
        context.pi, context.vs_pos);
}

void ZStrategy::cancelBuy() {
    // TODO:加入Quote逻辑后再搞

}

void ZStrategy::cancelSell() {
    // TODO:加入Quote逻辑后再搞
}

void ZStrategy::on_signal(const MSMarketDataField * market_data, double signal, short source, long rcv_time) {
    if (market_data == nullptr) {
        return;
    }
    last_ob_ptr = const_cast<MSMarketDataField*>(market_data);
    context.market_receive_realtime_ns = rcv_time > 0
        ? static_cast<uint64_t>(rcv_time) * 1000ULL : 0ULL;
    if (context.last_ob == nullptr) {
        context.last_ob = market_data;
        context.curr_ob = market_data;
        startup_signal_count_ = 1;
        if (sze_position_risk::StartupWarmupActive(
                startup_signal_count_, startup_warmup_signal_count_)) {
            calcTheo(signal);
        }
        return;
    }

    context.curr_ob = market_data;
    ++startup_signal_count_;
    const bool startup_warmup_active = sze_position_risk::StartupWarmupActive(
        startup_signal_count_, startup_warmup_signal_count_);
    if (startup_warmup_active) {
        // Keep the prediction/theoretical-price path alive during warmup, but
        // do not invoke the T0 order decision path or the test-order trigger.
        calcTheo(signal);
        KF_LOG_INFO(logger, "[SZEWarmup] instrument=" << mTradeInstrument
            << " sample=" << startup_signal_count_
            << " prediction=" << signal << " trading=0");
        context.last_ob = market_data;
        return;
    }
    if (test_order_.enabled &&
        startup_signal_count_ >= startup_warmup_signal_count_ + test_order_.trigger_after_signals) {
        maybe_send_test_order();
    }
    calcTheo(signal);
    latency_trace_.signal_decision_mono_ns = util ? static_cast<uint64_t>(util->get_nano()) : 0ULL;
    handleT0();
    context.last_ob = market_data;
}

void ZStrategy::on_rtn_order(const LFRtnOrderField *data, int request_id, short source, long rcv_time) {
    if (data == nullptr) {
        return;
    }

    const long order_ref = data->OrderRef;
    const long order_state_key = OrderStateKey(request_id, order_ref);
    const int cum_traded = std::max(0, data->VolumeTraded);
    const int leaves_qty = std::max(0, data->VolumeTotal);
    const auto traded_it = order_ref_last_traded_.find(order_state_key);
    const auto leaves_it = order_ref_last_leaves_.find(order_state_key);
    const int prev_cum_traded =
        traded_it == order_ref_last_traded_.end() ? 0 : std::max(0, traded_it->second);
    const int prev_leaves_qty =
        leaves_it == order_ref_last_leaves_.end() ? -1 : std::max(0, leaves_it->second);
    const int traded_delta = std::max(0, cum_traded - prev_cum_traded);

    int cancel_delta = 0;
    if (prev_leaves_qty >= 0) {
        cancel_delta = std::max(0, prev_leaves_qty - leaves_qty - traded_delta);
    }
    int pending_release = traded_delta + cancel_delta;
    const bool terminal_status =
        data->OrderStatus == LF_CHAR_AllTraded ||
        data->OrderStatus == LF_CHAR_PartTradedNotQueueing ||
        data->OrderStatus == LF_CHAR_NoTradeNotQueueing ||
        data->OrderStatus == LF_CHAR_Canceled ||
        data->OrderStatus == LF_CHAR_Error;
    if (terminal_status && traded_delta == 0 &&
        terminal_order_state_keys_.find(order_state_key) != terminal_order_state_keys_.end()) {
        return;
    }
    if (terminal_status) {
        const int original_qty = std::max(0, data->VolumeTotalOriginal);
        const int terminal_release = prev_leaves_qty >= 0
            ? prev_leaves_qty
            : std::max(leaves_qty + traded_delta, original_qty - prev_cum_traded);
        pending_release = std::max(pending_release, terminal_release);
    }

    order_ref_last_traded_[order_state_key] = std::max(prev_cum_traded, cum_traded);
    order_ref_last_leaves_[order_state_key] = leaves_qty;

    if (pending_release > 0) {
        if (data->Direction == LF_CHAR_Buy) {
            context.vl_pos = std::max(0, context.vl_pos - pending_release);
        } else {
            context.vs_pos = std::max(0, context.vs_pos - pending_release);
        }
    }
    if (traded_delta > 0) {
        order_fill_from_order_seen_[order_state_key] = true;
        if (data->Direction == LF_CHAR_Buy) {
            context.pi += traded_delta;
            context.cum_buy += traded_delta;
        } else {
            context.pi -= traded_delta;
            context.cum_sell += traded_delta;
            i_params.shortable = std::max(0, i_params.shortable - traded_delta);
        }
    }
    if (terminal_status) {
        terminal_order_state_keys_.insert(order_state_key);
        order_ref_last_leaves_.erase(order_state_key);
        if (request_id >= 0) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            cancel_pending_request_ids_.erase(request_id);
        }
    }

    if (pending_release > 0 || traded_delta > 0 || terminal_status) {
        double cur_position = getCurPosition();
        KF_LOG_INFO(logger, "[OrderRtn] InstrumentID: " << data->InstrumentID
                    << ", RequestID: " << request_id
                    << ", OrderRef: " << order_ref
                    << ", Status: " << data->OrderStatus
                    << ", Direction: " << (data->Direction == LF_CHAR_Buy ? "Buy" : "Sell")
                    << ", VolumeTraded: " << data->VolumeTraded
                    << ", PrevCumTraded: " << prev_cum_traded
                    << ", TradedDelta: " << traded_delta
                    << ", PrevLeaves: " << prev_leaves_qty
                    << ", LeavesNow: " << leaves_qty
                    << ", CancelDelta: " << cancel_delta
                    << ", PendingRelease: " << pending_release
                    << ", Price: " << data->LimitPrice
                    << ", Position: " << context.pi
                    << ", vl_pos: " << context.vl_pos
                    << ", vs_pos: " << context.vs_pos
                    << ", Cumulative Buy: " << context.cum_buy
                    << ", Cumulative Sell: " << context.cum_sell
                    << ", CurrentPosition: " << cur_position
                    );
    }
}

void ZStrategy::on_rtn_trade(const LFRtnTradeField *data, int request_id, short source, long rcv_time) {
    if (data == nullptr) {
        return;
    }

    const long order_ref = data->OrderRef;
    const long order_state_key = OrderStateKey(request_id, order_ref);
    const auto order_fill_it = order_fill_from_order_seen_.find(order_state_key);
    if (order_fill_it != order_fill_from_order_seen_.end() && order_fill_it->second) {
        return;
    }

    const int trade_volume = std::max(0, data->Volume);
    if (trade_volume <= 0) {
        return;
    }
    std::string trade_key;
    if (data->TradeID[0] != '\0') {
        trade_key.assign(data->TradeID);
    } else {
        std::ostringstream key_builder;
        key_builder << order_ref << '|' << data->Volume << '|' << data->Price
                    << '|' << data->TradingDay << '|' << data->TradeTime << '|' << data->Direction;
        trade_key = key_builder.str();
    }
    if (!seen_trade_keys_.insert(trade_key).second) {
        return;
    }

    const auto traded_it = order_ref_last_traded_.find(order_state_key);
    const auto leaves_it = order_ref_last_leaves_.find(order_state_key);
    const int prev_cum_traded =
        traded_it == order_ref_last_traded_.end() ? 0 : std::max(0, traded_it->second);
    const int prev_leaves_qty =
        leaves_it == order_ref_last_leaves_.end() ? -1 : std::max(0, leaves_it->second);
    const int new_cum_traded = prev_cum_traded + trade_volume;
    const int new_leaves_qty =
        prev_leaves_qty < 0 ? -1 : std::max(0, prev_leaves_qty - trade_volume);
    order_ref_last_traded_[order_state_key] = new_cum_traded;
    if (new_leaves_qty >= 0) {
        order_ref_last_leaves_[order_state_key] = new_leaves_qty;
    }

    const bool is_buy = data->Direction == LF_CHAR_Buy;
    if (is_buy) {
        context.vl_pos = std::max(0, context.vl_pos - trade_volume);
        context.pi += trade_volume;
        context.cum_buy += trade_volume;
    } else {
        context.vs_pos = std::max(0, context.vs_pos - trade_volume);
        context.pi -= trade_volume;
        context.cum_sell += trade_volume;
        i_params.shortable = std::max(0, i_params.shortable - trade_volume);
    }

    KF_LOG_INFO(logger, "[TradeRtnApply] InstrumentID: " << data->InstrumentID
                << ", RequestID: " << request_id
                << ", OrderRef: " << order_ref
                << ", Direction: " << (is_buy ? "Buy" : "Sell")
                << ", TradeVolume: " << trade_volume
                << ", Price: " << data->Price
                << ", PrevCumTraded: " << prev_cum_traded
                << ", NewCumTraded: " << new_cum_traded
                << ", PrevLeaves: " << prev_leaves_qty
                << ", NewLeaves: " << new_leaves_qty
                << ", Position: " << context.pi
                << ", vl_pos: " << context.vl_pos
                << ", vs_pos: " << context.vs_pos
                << ", shortable: " << i_params.shortable
                << ", Cumulative Buy: " << context.cum_buy
                << ", Cumulative Sell: " << context.cum_sell
                << ", CurrentPosition: " << getCurPosition());
}
