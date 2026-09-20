//
// Created by Administrator on 25-9-14.
//


#include "common/strategy/ZStrategy.h"
#include "common/strategy/sze_position_risk.h"
#include "common/strategy/common.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <limits>

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

bool valid_divisor(double value) {
    return std::isfinite(value) && value > 1.0e-12;
}

bool order_quantity(double theoretical, double available, double notional_cap,
                    std::int32_t* result) {
    if (!std::isfinite(theoretical) || !std::isfinite(available) ||
        !std::isfinite(notional_cap) || theoretical < 0 || available < 0 || notional_cap < 0)
        return false;
    const double quantity = std::min(theoretical, std::min(available, notional_cap));
    if (quantity > std::numeric_limits<std::int32_t>::max()) return false;
    *result = static_cast<std::int32_t>(quantity);
    return true;
}

#define Z_LOG_INFO(expr) do { \
    std::ostringstream _z_log_stream; _z_log_stream << expr; \
    if (execution_) execution_->log("INFO", _z_log_stream.str()); \
} while (0)
#define Z_LOG_ERROR(expr) do { \
    std::ostringstream _z_log_stream; _z_log_stream << expr; \
    if (execution_) execution_->log("ERROR", _z_log_stream.str()); \
} while (0)
}

ZStrategy::ZStrategy(const std::string &InstrumentID,
    const InsParams &ins_params,
    json &config,
    const std::shared_ptr<StrategyExecution>& execution)
    : mTradeInstrument(InstrumentID), j_config(config), execution_(execution),
      last_ob_ptr(nullptr) {
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
    const char* routing_key = ExchangeID == "SSE"
        ? "sse_order_routing" : "sze_order_routing";
    if (config.find(routing_key) != config.end() &&
        config[routing_key].is_object()) {
        const json& routing = config[routing_key];
        if (routing.find("enabled") != routing.end() && routing["enabled"].is_boolean()) {
            routing_enabled_ = routing["enabled"].get<bool>();
        }
        if (routing.find("mode") != routing.end() && routing["mode"].is_string()) {
            virtual_routing_ = routing["mode"].get<std::string>() == "virtual";
        }
        recovery_routing_ = routing.value("input_mode", std::string()) == "recovery_handoff";
        max_order_volume_ = routing.value("max_order_volume", 0);
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
    Z_LOG_INFO("[SZEWarmup] instrument=" << mTradeInstrument
        << " prediction_only_samples=" << startup_warmup_signal_count_);

    const char* test_order_key = ExchangeID == "SSE"
        ? "sse_test_order" : "sze_test_order";
    if (config.find(test_order_key) != config.end() &&
        config[test_order_key].is_object()) {
        const json& test = config[test_order_key];
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
        const std::string symbol_key = mTradeInstrument +
            (ExchangeID == "SSE" ? ".SH" : ".SZ");
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
    if (execution_ && execution_->managed()) {
        oms::Position position;
        if (!execution_->read_position(td_source_, mTradeInstrument, ExchangeID, &position) ||
            position.total != total_position || position.sellable != available_position || !refresh_position())
            throw std::runtime_error("startup position must match the authoritative OMS snapshot");
        return;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    const sze_position_risk::StartupPosition startup =
        sze_position_risk::NormalizeStartupPosition(
            i_params.static_position, total_position, available_position);
    context.pi = startup.delta_from_static;
    i_params.last_position = startup.delta_from_static;
    i_params.shortable = startup.available;
    Z_LOG_INFO("[SZEPosSync] InstrumentID=" << mTradeInstrument
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
    const long long now = execution_ ? execution_->now_ns() : 0;
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
        Z_LOG_INFO("[RiskReject] InstrumentID=" << mTradeInstrument
            << ", request_id=" << request_id
            << ", error_id=" << error_id
            << ", reason=" << RiskCodeText(error_id)
            << ", side=" << (order.Direction == BUY ? "Buy" : "Sell")
            << ", volume=" << order.Volume
            << ", price=" << order.Price);
    }
}

int ZStrategy::insertOrder(RT_Order order, const std::string& signal_id) {
    if (!routing_enabled_) {
        Z_LOG_ERROR("[SZEOrderBlocked] InstrumentID=" << mTradeInstrument
            << ", reason=routing_disabled");
        return -1;
    }
    if (virtual_routing_) {
        Z_LOG_INFO("[SZEVirtualOrderIntent] InstrumentID=" << mTradeInstrument
            << ", Side=" << (order.Direction == BUY ? "Buy" : "Sell")
            << ", Volume=" << order.Volume
            << ", Price=" << order.Price
            << ", Exchange=" << ExchangeID
            << ", td_source=" << td_source_);
        return -1;
    }
    if (recovery_routing_) {
        // T0 permits intraday buy/sell rotation. max_position is a per-order
        // safety cap; net-position eligibility is checked before insertion.
        if (max_order_volume_ <= 0 || order.Volume > max_order_volume_ ||
            (max_position_ > 0 && order.Volume > max_position_)) {
            Z_LOG_ERROR("[SZEOrderBlocked] InstrumentID=" << mTradeInstrument
                << ", reason=position_or_volume_limit"
                << ", volume=" << order.Volume
                << ", max_order_volume=" << max_order_volume_
                << ", max_position_per_order=" << max_position_);
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
        Z_LOG_INFO("[VirtualOrderIntent] InstrumentID=" << mTradeInstrument
            << ", Side=" << (order.Direction == BUY ? "Buy" : "Sell")
            << ", Volume=" << order.Volume
            << ", Price=" << order.Price
            << ", Type=FAK"
            << ", Exchange=" << ExchangeID
            << ", td_source=" << td_source_
            << ", action=skip_insert_limit_order");
        return -1;
#else
        if (!execution_ || !execution_->managed() || !execution_->permits_new_orders()) {
            Z_LOG_ERROR("[OrderBlocked] InstrumentID=" << mTradeInstrument
                        << ", reason=execution_unhealthy");
            return -1;
        }
        request_id = execution_->submit_limit_then_cancel(td_source_, mTradeInstrument,
            ExchangeID, order.Price, static_cast<int>(order.Volume), direction, offsetFlag, 1001, signal_id);
        refresh_position();
        if (request_id < 0) {
            on_order_reject(request_id, order);
        }
#endif
    }
    return request_id;
}

void ZStrategy::maybe_send_test_order(const std::string& signal_id) {
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
        Z_LOG_ERROR("[SZTestOrder] skipped invalid market snapshot price=" << price
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
    const int request_id = insertOrder(order, signal_id);
    Z_LOG_INFO("[SZTestOrder] submitted instrument=" << mTradeInstrument
        << " side=" << (order.Direction == BUY ? "buy" : "sell")
        << " price=" << order.Price << " volume=" << order.Volume
        << " request_id=" << request_id
        << " cancel_delay_ms=" << test_order_.cancel_delay_ms);
    if (request_id >= 0 && test_order_.cancel_delay_ms > 0) {
        delay_cancel_order(request_id, test_order_.cancel_delay_ms);
    }
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

    if (market_time_minutes < 930) {
        g_params.position_limit = 0.0;
    } else if (market_time_minutes < 932) {
        g_params.position_limit = 0.3 * g_params.position_limit_base_line;
    } else if (market_time_minutes < 934) {
        g_params.position_limit = 0.5 * g_params.position_limit_base_line;
    } else if (market_time_minutes >= 1430) {
        g_params.position_limit = 0.6 * g_params.position_limit_base_line;
    } else {
        g_params.position_limit = g_params.position_limit_base_line;
    }
}

void ZStrategy::cancel_order(int request_id) {
    if (request_id < 0) {
        return;
    }
    if (!routing_enabled_ || virtual_routing_) {
        Z_LOG_INFO("[SZEVirtualCancelIntent] InstrumentID=" << mTradeInstrument
            << ", request_id=" << request_id
            << ", td_source=" << td_source_);
        return;
    }
#ifdef T0_VIRTUAL_TRADING
    Z_LOG_INFO("[VirtualCancelIntent] InstrumentID=" << mTradeInstrument
        << ", request_id=" << request_id
        << ", td_source=" << td_source_
        << ", action=skip_cancel_order");
    return;
#else
    const int cancel_ret = execution_ ? execution_->cancel(td_source_, request_id) : -1;
    if (cancel_ret < 0) {
        const int error_id = NormalizeRiskCode(cancel_ret);
        if (error_id == kRiskErrShortOfCancelNum) {
            Z_LOG_INFO("[RiskCancel] InstrumentID=" << mTradeInstrument
                << ", request_id=" << request_id
                << ", error_id=" << error_id
                << ", reason=" << RiskCodeText(error_id));
        } else {
            Z_LOG_INFO("[RiskCancel] InstrumentID=" << mTradeInstrument
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
    if (!routing_enabled_ || virtual_routing_) return;
#ifndef T0_VIRTUAL_TRADING
    if (execution_) execution_->schedule_cancel(td_source_, request_id, delay_ms);
#endif
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


void ZStrategy::handleT0(const std::string& signal_id) {
    cancelBuy();
    cancelSell();
    const long long now = execution_ ? execution_->now_ns() : 0;
    if (context.CanBuy() && can_send_order(BUY, now)) {
        hitBuy(signal_id);
    }

    if (context.CanSell() && can_send_order(SELL, now)) {
        hitSell(signal_id);
    }
}

void ZStrategy::hitBuy(const std::string& signal_id) {
    int32_t hit_buy_qty = 0;
    auto cur_ob = &(context.last_ob);
    (void)cur_ob;
    if (context.curr_ob == nullptr ||
        !valid_divisor(context.curr_ob->AskPrice1) ||
        !valid_divisor(theo_.unitbias)) {
        Z_LOG_ERROR("[InvalidSignal] instrument=" << mTradeInstrument
                    << ", side=Buy, reason=zero_or_nonfinite_divisor");
        return;
    }
    double buy_margin = theo_.hit_buy_theo / context.curr_ob->AskPrice1 - 1;
    if (buy_margin > 0) {
        if (!order_quantity(buy_margin / theo_.unitbias, context.curr_ob->AskVolume1,
                            i_params.max_order_size / context.curr_ob->AskPrice1, &hit_buy_qty)) return;
        const int32_t lot = i_params.vol_unit > 0 ? i_params.vol_unit : vol_unit;
        hit_buy_qty = std::min(hit_buy_qty / lot * lot,maxCanBuy());
        if (hit_buy_qty < lot) {
            return;
        }

        RT_Order order;
        order.Volume = hit_buy_qty;
        order.Price = context.curr_ob->AskPrice1;
        order.Direction = BUY;
        order.Type=FAK;
        int request_id = insertOrder(order, signal_id);
        double cur_position = getCurPosition();
        Z_LOG_INFO("[HitBuy] InstrumentID: " << mTradeInstrument
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
            << ", HitBuyTheo: " << theo_.hit_buy_theo
            << ", vl_pos: " << context.vl_pos
            << ", vs_pos: " << context.vs_pos
            << ", CurrentPosition: " << cur_position);
    }
}


void ZStrategy::hitSell(const std::string& signal_id) {
    std::int32_t hit_sell_qty = 0;
    auto cur_ob = &(context.last_ob);
    (void)cur_ob;
    if (context.curr_ob == nullptr ||
        !valid_divisor(context.curr_ob->BidPrice1) ||
        !valid_divisor(theo_.hit_sell_theo) ||
        !valid_divisor(theo_.unitbias)) {
        Z_LOG_ERROR("[InvalidSignal] instrument=" << mTradeInstrument
                    << ", side=Sell, reason=zero_or_nonfinite_divisor");
        return;
    }
    double sell_margin =  context.curr_ob->BidPrice1 / theo_.hit_sell_theo - 1;
    if (sell_margin > 0) {
        if (!order_quantity(sell_margin / theo_.unitbias, context.curr_ob->BidVolume1,
                            i_params.max_order_size / context.curr_ob->BidPrice1, &hit_sell_qty)) return;
        const int32_t lot = i_params.vol_unit > 0 ? i_params.vol_unit : vol_unit;
        hit_sell_qty = std::min(hit_sell_qty / lot * lot,maxCanSell());
        if (hit_sell_qty < lot) {
            return;
        }
        RT_Order order;
        order.Volume = hit_sell_qty;
        order.Price = context.curr_ob->BidPrice1;
        order.Direction = SELL;
        order.Type=FAK;

        int request_id = insertOrder(order, signal_id);
        double cur_position = getCurPosition();
        Z_LOG_INFO("[HitSell] InstrumentID: " << mTradeInstrument
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
    if (market_data == nullptr || !std::isfinite(signal)) {
        return;
    }
    if (execution_ && execution_->managed() && !refresh_position()) return;
    last_ob_ptr = const_cast<MSMarketDataField*>(market_data);
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
    const std::string signal_id = mTradeInstrument + ":" +
        std::to_string(startup_signal_count_) + ":" + std::to_string(rcv_time);
    const bool startup_warmup_active = sze_position_risk::StartupWarmupActive(
        startup_signal_count_, startup_warmup_signal_count_);
    if (startup_warmup_active) {
        // Keep the prediction/theoretical-price path alive during warmup, but
        // do not invoke the T0 order decision path or the test-order trigger.
        calcTheo(signal);
        Z_LOG_INFO("[SZEWarmup] instrument=" << mTradeInstrument
            << " sample=" << startup_signal_count_
            << " prediction=" << signal << " trading=0");
        context.last_ob = market_data;
        return;
    }
    if (test_order_.enabled &&
        startup_signal_count_ >= startup_warmup_signal_count_ + test_order_.trigger_after_signals) {
        maybe_send_test_order(signal_id);
    }
    calcTheo(signal);
    handleT0(signal_id);
    context.last_ob = market_data;
}

bool ZStrategy::refresh_position() {
    oms::Position position;
    if (!execution_ || !execution_->read_position(td_source_, mTradeInstrument, ExchangeID, &position)) return false;
    const long long maximum = std::numeric_limits<int32_t>::max();
    if (position.total < 0 || position.total > maximum || position.sellable < 0 || position.sellable > maximum ||
        position.working_buy < 0 || position.working_buy > maximum || position.working_sell < 0 ||
        position.working_sell > maximum || position.bought > maximum || position.sold > maximum) return false;
    context.pi = static_cast<int32_t>(position.total - i_params.static_position);
    context.vl_pos = static_cast<int32_t>(position.working_buy);
    context.vs_pos = static_cast<int32_t>(position.working_sell);
    context.cum_buy = static_cast<int32_t>(position.bought);
    context.cum_sell = static_cast<int32_t>(position.sold);
    i_params.shortable = static_cast<int32_t>(position.sellable);
    return true;
}

void ZStrategy::on_rtn_order(const LFRtnOrderField*, int, short, long) {
    // LF callbacks lack the OMS epoch and coverage contract; they never mutate risk state.
}

void ZStrategy::on_rtn_trade(const LFRtnTradeField*, int, short, long) {}
