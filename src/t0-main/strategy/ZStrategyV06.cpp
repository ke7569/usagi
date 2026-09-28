#include "ZStrategy.h"
#include "StrategyBase.h"
#include <algorithm>
#include <cmath>
#include <limits>

void ZStrategy::bind_v06_account(const std::shared_ptr<V06AccountState>& account) {
    v06_account_ = account;
    if (account) {
        account->register_instrument(mTradeInstrument, i_params.static_position);
        if (account->blocked(mTradeInstrument))
            KF_LOG_INFO(logger, "[V06DailyBlockLoaded] instrument=" << mTradeInstrument
                << " prediction_disabled=1 trading_disabled_for_day=1");
    }
}

void ZStrategy::halt_v06_for_day(const std::string& reason) {
    if (!v06_account_) return;
    std::lock_guard<std::recursive_mutex> lock(v06_account_->mutex);
    const bool first = !v06_account_->blocked(mTradeInstrument);
    v06_account_->block(mTradeInstrument, reason);
    for (const auto& item : v06_orders_)
        if (!item.second.terminal) v06_cancel(item.first);
    if (first) KF_LOG_INFO(logger, "[V06DailyBlock] instrument=" << mTradeInstrument
        << " reason=" << reason << " trading_disabled_for_day=1");
}

void ZStrategy::on_v06_signal(const MSMarketDataField* md,
                            const std::array<float, 4>& heads,
                            std::uint64_t exchange_us, short, long) {
    if (!v06_enabled_ || !v06_account_ || !md) return;
    std::lock_guard<std::recursive_mutex> lock(v06_account_->mutex);
    if (v06_account_->blocked(mTradeInstrument)) {
        halt_v06_for_day("daily quarantine");
        return;
    }
    if (md->BidPrice1 > 0 && md->AskPrice1 > 0 &&
        md->BidPrice1 > md->AskPrice1 + 1e-8) {
        halt_v06_for_day("crossed best bid/ask");
        return;
    }
    const auto global = v06_account_->skew(exchange_us, v06_config_);
    if (!routing_enabled_ || virtual_routing_) return;
    ++startup_signal_count_; // V06 uses exchange-time gates, no extra sample warmup.
    v06_strategy::PricingInput input;
    input.prediction_permille = heads[0];
    for (unsigned i = 0; i < 4; ++i) input.agreement_heads[i] = heads[i];
    input.bid_price = md->BidPrice1; input.ask_price = md->AskPrice1;
    input.last_price = md->LastPrice;
    if (!std::isfinite(md->BidVolume1) || !std::isfinite(md->AskVolume1) ||
        md->BidVolume1 < 0 || md->AskVolume1 < 0 ||
        md->BidVolume1 > std::numeric_limits<int>::max() ||
        md->AskVolume1 > std::numeric_limits<int>::max()) return;
    input.bid_volume = static_cast<int>(md->BidVolume1);
    input.ask_volume = static_cast<int>(md->AskVolume1);
    input.exchange_time_micros = exchange_us;
    input.static_position = i_params.static_position;
    input.current_position = context.pi;
    input.global_buy_skew_bps = global.buy_bps;
    input.global_sell_skew_bps = global.sell_bps;
    const int agreement = v06_strategy::strictAgreement(input.agreement_heads);
    KF_LOG_INFO(logger, "[V06Signal] instrument=" << mTradeInstrument << " exchange_us=" << exchange_us
        << " head15=" << heads[0] << " head30=" << heads[1] << " head60=" << heads[2] << " head120=" << heads[3]
        << " agreement=" << agreement << " relative_position=" << context.pi
        << " reserved_buy=" << context.vl_pos << " reserved_sell=" << context.vs_pos
        << " global_ready=" << global.valid << " global_buy_bps=" << global.buy_bps << " global_sell_bps=" << global.sell_bps);
    // Revoke an opening order when its entire remainder is no longer allowed.
    for (auto side : {v06_strategy::Side::Buy, v06_strategy::Side::Sell}) {
        const auto ids = v06_reservations_.openingOrdersToRevoke(side, context.pi, agreement,
                                                                v06_config_.lot_size);
        for (auto id : ids) v06_cancel(static_cast<int>(id));
    }
    if (!v06_strategy::isStrategyTime(exchange_us)) {
        for (const auto& order : v06_orders_) if (!order.second.terminal) v06_cancel(order.first);
        return;
    }
    // Account admission and an invalid pricing head block new intents while
    // still allowing the MH4 revocation above to reduce outstanding risk.
    if (!global.valid || !std::isfinite(heads[0])) return;
    v06_strategy::V06StrategyRules rules(v06_config_);
    auto decision = rules.price(input);
    if (!decision.valid) return;
    // Existing quotes are maintained against the current theoretical limit;
    // cancelling does not make their quantity available to a replacement.
    for (const auto& item : v06_orders_) {
        const V06Order& order = item.second;
        if (order.terminal || order.kind != v06_strategy::OrderKind::Quote) continue;
        if ((order.buy && order.price > decision.quote_buy_theo) ||
            (!order.buy && order.price < decision.quote_sell_theo)) v06_cancel(item.first);
    }
    if (decision.signal == v06_strategy::Signal::None) return;
    const auto side = decision.side;
    input.reserved_same_side = v06_reservations_.reservedVolume(side);
    v06_strategy::PositionClampInput clamp;
    clamp.requested = decision.speedbag_volume; clamp.side = side; clamp.kind = decision.kind;
    clamp.current_position = context.pi; clamp.static_position = i_params.static_position;
    clamp.opening_position = i_params.static_position + i_params.last_position;
    clamp.long_position = context.cum_buy; clamp.short_position = context.cum_sell;
    clamp.dirty_buy_hit = v06_reservations_.reservedVolume(v06_strategy::Side::Buy, v06_strategy::OrderKind::Hit);
    clamp.dirty_buy_quote = v06_reservations_.reservedVolume(v06_strategy::Side::Buy, v06_strategy::OrderKind::Quote);
    clamp.dirty_sell_hit = v06_reservations_.reservedVolume(v06_strategy::Side::Sell, v06_strategy::OrderKind::Hit);
    clamp.dirty_sell_quote = v06_reservations_.reservedVolume(v06_strategy::Side::Sell, v06_strategy::OrderKind::Quote);
    clamp.exchange_time_micros = exchange_us;
    long long qty = v06_strategy::ordinaryAllowedVolume(clamp, v06_config_);
    const long long ordinary_qty = qty;
    qty = v06_strategy::allowedVolume(side, qty, context.pi,
                                     input.reserved_same_side, agreement, v06_config_.lot_size);
    if (side == v06_strategy::Side::Sell)
        qty = std::min<long long>(qty, std::max<long long>(0, i_params.shortable - input.reserved_same_side));
    // The real account's pre-existing launch cap remains an additional bound.
    if (max_position_ > 0) qty = std::min<long long>(qty, max_position_);
    if (i_params.max_order_size > 0 && decision.order_price > 0)
        qty = std::min<long long>(qty, static_cast<long long>(i_params.max_order_size / decision.order_price));
    qty = qty / v06_config_.lot_size * v06_config_.lot_size;
    const bool valid_price = std::isfinite(decision.order_price) && decision.order_price > 0 &&
        (v06_upper_price_ <= 0 || decision.order_price <= v06_upper_price_ + 1e-8) &&
        (v06_lower_price_ <= 0 || decision.order_price >= v06_lower_price_ - 1e-8);
    KF_LOG_INFO(logger, "[V06Decision] instrument=" << mTradeInstrument << " exchange_us=" << exchange_us
        << " agreement=" << agreement << " side=" << (side == v06_strategy::Side::Buy ? "buy" : "sell")
        << " kind=" << (decision.kind == v06_strategy::OrderKind::Hit ? "hit" : "quote")
        << " price=" << decision.order_price << " requested=" << decision.speedbag_volume
        << " ordinary=" << ordinary_qty << " reserved_same_side=" << input.reserved_same_side
        << " allowed=" << qty << " price_band_valid=" << valid_price);
    if (qty <= 0 || qty > std::numeric_limits<int>::max() ||
        !valid_price ||
        !can_send_order(side == v06_strategy::Side::Buy ? BUY : SELL, util->get_nano())) return;
    v06_submit(decision, static_cast<int>(qty));
}

int ZStrategy::v06_submit(const v06_strategy::PricingOutput& d, int quantity) {
    if (!v06_account_ || v06_account_->blocked(mTradeInstrument)) return -1;
    const bool buy = d.side == v06_strategy::Side::Buy;
    if (i_params.static_position == 0) {
        const long long reserved = v06_reservations_.reservedVolume(v06_strategy::Side::Sell);
        if (buy || quantity <= 0 || quantity > context.pi - reserved ||
            quantity > i_params.shortable - reserved) return -1;
    }
    v06_submitting_ = true;
    const int id = util->insert_limit_order(td_source_, mTradeInstrument, ExchangeID,
        d.order_price, quantity, buy ? LF_CHAR_Buy : LF_CHAR_Sell,
        buy ? LF_CHAR_Open : LF_CHAR_Close);
    if (id >= 0) {
        V06Order order; order.original = quantity; order.buy = buy;
        order.price = d.order_price; order.kind = d.kind;
        if (!v06_orders_.insert(std::make_pair(id, order)).second ||
            !v06_reservations_.reserve(id, d.side, d.kind, quantity, true)) v06_account_->invalidate();
        if (buy) context.vl_pos += quantity; else context.vs_pos += quantity;
    } else {
        RT_Order rejected(d.order_price, quantity, buy ? BUY : SELL, d.kind == v06_strategy::OrderKind::Hit ? FAK : LMP);
        on_order_reject(id, rejected);
    }
    v06_submitting_ = false;
    auto orders = std::move(v06_early_orders_); v06_early_orders_.clear();
    auto trades = std::move(v06_early_trades_); v06_early_trades_.clear();
    for (const auto& item : orders) v06_order_return(&item.first, item.second);
    for (const auto& item : trades) v06_trade_return(&item.first, item.second);
    if (id >= 0 && d.kind == v06_strategy::OrderKind::Hit) {
        BLCallback callback = std::bind(&ZStrategy::v06_cancel, this, id);
        util->insert_callback(util->get_nano() + v06_config_.hit_timeout_micros * 1000LL, callback);
    }
    KF_LOG_INFO(logger, "[V06OrderIntent] instrument=" << mTradeInstrument << " request_id=" << id
        << " side=" << (buy ? "buy" : "sell") << " quantity=" << quantity
        << " price=" << d.order_price << " kind=" << (d.kind == v06_strategy::OrderKind::Hit ? "hit" : "quote"));
    return id;
}

void ZStrategy::v06_cancel(int id) {
    if (!v06_account_) return;
    std::lock_guard<std::recursive_mutex> lock(v06_account_->mutex);
    auto found = v06_orders_.find(id);
    if (found == v06_orders_.end() || found->second.terminal) return;
    // Keep the reservation through a lost/rejected cancel response, and retry
    // with a bounded interval until the order itself reaches a terminal state.
    const long long now = util->get_nano();
    if (found->second.cancel_pending && now < found->second.cancel_retry_ns) return;
    if (!v06_reservations_.requestCancel(id)) return;
    found->second.cancel_pending = true;
    found->second.cancel_retry_ns = now + 5000000000LL;
    const int result = util->cancel_order(td_source_, id);
    KF_LOG_INFO(logger, "[V06CancelRequest] instrument=" << mTradeInstrument << " request_id=" << id
        << " result=" << result << " remaining_reserved=" << found->second.original - found->second.cumulative
        << " now_ns=" << now);
    if (result < 0) {
        found->second.cancel_retry_ns = now + 1000000000LL;
    }
    BLCallback callback = std::bind(&ZStrategy::v06_cancel, this, id);
    util->insert_callback(found->second.cancel_retry_ns, callback);
}

void ZStrategy::v06_order_return(const LFRtnOrderField* data, int id) {
    if (!data || !v06_account_) return;
    std::lock_guard<std::recursive_mutex> lock(v06_account_->mutex);
    if (v06_submitting_) { v06_early_orders_.push_back(std::make_pair(*data, id)); return; }
    auto found = v06_orders_.find(id);
    if (found == v06_orders_.end()) { v06_account_->invalidate(); return; }
    V06Order& order = found->second;
    const int cumulative = std::max(0, data->VolumeTraded);
    if (cumulative > order.original) { v06_account_->invalidate(); return; }
    if (cumulative < order.cumulative) return;
    const int delta = cumulative - order.cumulative;
    order.cumulative = cumulative;
    v06_account_->observe_cumulative(mTradeInstrument + ":" + std::to_string(id), cumulative);
    if (order.buy) { context.pi += delta; context.cum_buy += delta; }
    else { context.pi -= delta; context.cum_sell += delta; i_params.shortable = std::max(0, i_params.shortable - delta); }
    const bool terminal = data->OrderStatus == LF_CHAR_AllTraded ||
        data->OrderStatus == LF_CHAR_PartTradedNotQueueing ||
        data->OrderStatus == LF_CHAR_NoTradeNotQueueing ||
        data->OrderStatus == LF_CHAR_Canceled || data->OrderStatus == LF_CHAR_Error;
    order.terminal = order.terminal || terminal;
    const auto status = order.terminal
        ? (cumulative == order.original ? v06_strategy::ReservationStatus::Filled : v06_strategy::ReservationStatus::Cancelled)
        : (order.cancel_pending ? v06_strategy::ReservationStatus::CancelPending
           : cumulative ? v06_strategy::ReservationStatus::PartialFill : v06_strategy::ReservationStatus::Active);
    v06_reservations_.update(id, status, order.terminal ? 0 : order.original - cumulative);
    context.vl_pos = static_cast<int>(v06_reservations_.reservedVolume(v06_strategy::Side::Buy));
    context.vs_pos = static_cast<int>(v06_reservations_.reservedVolume(v06_strategy::Side::Sell));
    KF_LOG_INFO(logger, "[V06OrderReturn] instrument=" << mTradeInstrument << " request_id=" << id
        << " broker_status=" << data->OrderStatus << " cumulative=" << cumulative << " delta=" << delta << " terminal=" << order.terminal
        << " cancel_pending=" << order.cancel_pending << " relative_position=" << context.pi
        << " reserved_buy=" << context.vl_pos << " reserved_sell=" << context.vs_pos);
}

void ZStrategy::v06_trade_return(const LFRtnTradeField* data, int id) {
    if (!data || !v06_account_) return;
    std::lock_guard<std::recursive_mutex> lock(v06_account_->mutex);
    if (v06_submitting_) { v06_early_trades_.push_back(std::make_pair(*data, id)); return; }
    auto found = v06_orders_.find(id);
    if (found == v06_orders_.end()) { v06_account_->invalidate(); return; }
    // Quantity is reconciled from the same cumulative watermark irrespective
    // of whether the trade callback or order callback arrives first.
    LFRtnOrderField report = {};
    report.VolumeTraded = data->VolumeTraded;
    report.VolumeTotalOriginal = found->second.original;
    report.VolumeTotal = data->VolumeTotal;
    report.OrderStatus = data->VolumeTraded >= found->second.original ? LF_CHAR_AllTraded : LF_CHAR_PartTradedQueueing;
    v06_order_return(&report, id);
    if (!data->TradeID[0] || !data->OrderSysID[0]) { v06_account_->invalidate(); return; }
    const auto bounded = [](const char* text, std::size_t size) {
        std::size_t length = 0;
        while (length < size && text[length]) ++length;
        return std::string(text, length);
    };
    const std::string key = bounded(data->TradingDay, sizeof(data->TradingDay)) + ":" +
        bounded(data->OrderSysID, sizeof(data->OrderSysID)) + ":" + bounded(data->TradeID, sizeof(data->TradeID));
    v06_account_->fill(key, found->second.buy, data->Volume, data->Price,
        mTradeInstrument + ":" + std::to_string(id));
}
