#include "common/market_data/legacy/shsz_full_orderbook_manager.h"

#include <cmath>
#include <cstring>

ShSzFullOrderBookManager::PendingMarketOrder::PendingMarketOrder()
    : active(false),
      is_sell(false),
      quote_tag(0),
      volume(0),
      create_time_ms(0),
      resolved_price(0) {
}

ShSzFullOrderBookManager::PendingMarketOrderSnapshot::PendingMarketOrderSnapshot()
    : active(false),
      is_sell(false),
      quote_tag(0),
      volume(0),
      create_time_ms(0),
      resolved_price(0) {
}

ShSzFullOrderBookManager::InstrumentSnapshot::InstrumentSnapshot()
    : valid(false),
      pending_market_order(),
      book_state() {
}

ShSzFullOrderBookManager::InstrumentState::InstrumentState()
    : instrument_id(),
      instrument_id_value(0.0),
      engine(),
      pending_market_order() {
}

ShSzFullOrderBookManager::ShSzFullOrderBookManager() {
    mInstrumentStates.reserve(4096);
}

void ShSzFullOrderBookManager::process_order(const LFL2OrderField* order_data) {
    if (order_data == 0) {
        return;
    }

    MarketType market_type = judge_market_type(order_data->InstrumentID);
    if (market_type == NONE) {
        return;
    }

    InstrumentState& state = ensure_instrument(order_data->InstrumentID);
    if (market_type == SH_STOCK_MARKET) {
        process_sh_stock_order(&state, order_data);
        return;
    }
    process_sz_stock_order(&state, order_data);
}

void ShSzFullOrderBookManager::process_trade(const LFL2TradeField* trade_data) {
    if (trade_data == 0) {
        return;
    }

    MarketType market_type = judge_market_type(trade_data->InstrumentID);
    if (market_type == NONE) {
        return;
    }

    InstrumentState& state = ensure_instrument(trade_data->InstrumentID);
    if (market_type == SH_STOCK_MARKET) {
        process_sh_stock_trade(&state, trade_data);
        return;
    }
    process_sz_stock_trade(&state, trade_data);
}

bool ShSzFullOrderBookManager::has_instrument(const char* instrument_id) const {
    if (instrument_id == 0) {
        return false;
    }
    return mInstrumentStates.find(instrument_id) != mInstrumentStates.end();
}

ShSzFullOrderBookEngine* ShSzFullOrderBookManager::get_engine(const char* instrument_id) {
    std::unordered_map<std::string, InstrumentState>::iterator it = mInstrumentStates.find(instrument_id == 0 ? "" : instrument_id);
    if (it == mInstrumentStates.end()) {
        return 0;
    }
    return &it->second.engine;
}

const ShSzFullOrderBookEngine* ShSzFullOrderBookManager::get_engine(const char* instrument_id) const {
    std::unordered_map<std::string, InstrumentState>::const_iterator it = mInstrumentStates.find(instrument_id == 0 ? "" : instrument_id);
    if (it == mInstrumentStates.end()) {
        return 0;
    }
    return &it->second.engine;
}

ShSzVisibleBook ShSzFullOrderBookManager::snapshot_visible_book(const char* instrument_id, uint32_t now_time_ms) const {
    const ShSzFullOrderBookEngine* engine = get_engine(instrument_id);
    if (engine == 0) {
        return ShSzVisibleBook();
    }
    return engine->snapshot_visible_book(now_time_ms);
}

ShSzFullOrderBookSummary ShSzFullOrderBookManager::snapshot_summary(const char* instrument_id,
                                                                    uint32_t now_time_ms) const {
    const ShSzFullOrderBookEngine* engine = get_engine(instrument_id);
    if (engine == 0) {
        return ShSzFullOrderBookSummary();
    }
    return engine->snapshot_summary(now_time_ms);
}

ShSzFullOb ShSzFullOrderBookManager::snapshot_full_orderbook_aggregate(const char* instrument_id,
                                                                       uint32_t now_time_ms) const {
    const ShSzFullOrderBookEngine* engine = get_engine(instrument_id);
    if (engine == 0) {
        return ShSzFullOb();
    }
    return engine->snapshot_full_orderbook_aggregate(now_time_ms);
}

ShSzFullOrderBookManager::InstrumentSnapshot ShSzFullOrderBookManager::snapshot_instrument(
    const char* instrument_id,
    uint32_t now_time_ms) const {
    InstrumentSnapshot snapshot;
    std::unordered_map<std::string, InstrumentState>::const_iterator it =
        mInstrumentStates.find(instrument_id == 0 ? "" : instrument_id);
    if (it == mInstrumentStates.end()) {
        return snapshot;
    }

    snapshot.valid = true;
    snapshot.pending_market_order = snapshot_pending_market_order(it->second.pending_market_order);
    snapshot.book_state = it->second.engine.snapshot_state(now_time_ms);
    return snapshot;
}

void ShSzFullOrderBookManager::clear() {
    mInstrumentStates.clear();
}

ShSzFullOrderBookManager::MarketType ShSzFullOrderBookManager::judge_market_type(const char* instrument_id) {
    if (instrument_id == 0) {
        return NONE;
    }
    if (instrument_id[0] == '6' && (instrument_id[1] == '0' || instrument_id[1] == '8')) {
        return SH_STOCK_MARKET;
    }
    if ((instrument_id[0] == '0' && instrument_id[1] == '0') ||
        (instrument_id[0] == '3' && instrument_id[1] == '0') ||
        (instrument_id[0] == '3' && instrument_id[1] == '1')) {
        return SZ_STOCK_MARKET;
    }
    return NONE;
}

ShSzFullOrderBookManager::InstrumentState& ShSzFullOrderBookManager::ensure_instrument(const char* instrument_id) {
    std::unordered_map<std::string, InstrumentState>::iterator it = mInstrumentStates.find(instrument_id);
    if (it != mInstrumentStates.end()) {
        return it->second;
    }

    InstrumentState state;
    state.instrument_id = instrument_id;
    state.instrument_id_value = parse_instrument_id_value(instrument_id);
    state.engine.set_instrument_id_value(state.instrument_id_value);
    std::pair<std::unordered_map<std::string, InstrumentState>::iterator, bool> inserted =
        mInstrumentStates.insert(std::make_pair(state.instrument_id, state));
    return inserted.first->second;
}

void ShSzFullOrderBookManager::process_sh_stock_order(InstrumentState* state, const LFL2OrderField* order_data) {
    const bool is_sell = order_data->OrderKind[0] == 'S';
    const int volume = static_cast<int>(order_data->Volume);
    const uint32_t event_time_ms = ShSzFullOrderBookEngine::parse_event_time_ms(order_data->OrderTime);
    const long quote_tag = order_data->OrderNo;

    if (order_data->OrdType[0] == 'A') {
        state->engine.add_order(quote_tag,
                                is_sell,
                                round_price(order_data->Price),
                                volume,
                                event_time_ms);
        return;
    }

    state->engine.cancel_order(quote_tag, volume);
}

void ShSzFullOrderBookManager::process_sz_stock_order(InstrumentState* state, const LFL2OrderField* order_data) {
    flush_pending_market_order_if_resolvable(state);

    const bool is_sell = order_data->OrderKind[0] == 'S';
    const int volume = static_cast<int>(order_data->Volume);
    const uint32_t event_time_ms = ShSzFullOrderBookEngine::parse_event_time_ms(order_data->OrderTime);
    const long quote_tag = order_data->ApplSeqNum;

    if (order_data->OrdType[0] == '1') {
        state->pending_market_order.active = true;
        state->pending_market_order.is_sell = is_sell;
        state->pending_market_order.quote_tag = quote_tag;
        state->pending_market_order.volume = volume;
        state->pending_market_order.create_time_ms = event_time_ms;
        state->pending_market_order.resolved_price = 0;
        return;
    }

    if (order_data->OrdType[0] == 'U') {
        const int best_price = is_sell ? state->engine.best_ask_price() : state->engine.best_bid_price();
        if (best_price <= 0) {
            return;
        }
        state->engine.add_order(quote_tag, is_sell, best_price, volume, event_time_ms);
        return;
    }

    state->engine.add_order(quote_tag,
                            is_sell,
                            round_price(order_data->Price),
                            volume,
                            event_time_ms);
}

void ShSzFullOrderBookManager::process_sh_stock_trade(InstrumentState* state, const LFL2TradeField* trade_data) {
    const int volume = static_cast<int>(trade_data->Volume);
    if (trade_data->OrderKind[0] == '4') {
        cancel_if_present(state, trade_data->BidApplSeqNum, volume);
        cancel_if_present(state, trade_data->OfferApplSeqNum, volume);
        return;
    }

    execute_if_present(state, trade_data->BidApplSeqNum, volume);
    execute_if_present(state, trade_data->OfferApplSeqNum, volume);
}

void ShSzFullOrderBookManager::process_sz_stock_trade(InstrumentState* state, const LFL2TradeField* trade_data) {
    try_resolve_pending_market_order(state, trade_data);

    const int volume = static_cast<int>(trade_data->Volume);
    if (trade_data->OrderKind[0] == '4') {
        flush_pending_market_order_if_resolvable(state);
        cancel_if_present(state, trade_data->BidApplSeqNum, volume);
        cancel_if_present(state, trade_data->OfferApplSeqNum, volume);
        return;
    }

    execute_if_present(state, trade_data->BidApplSeqNum, volume);
    execute_if_present(state, trade_data->OfferApplSeqNum, volume);
    flush_pending_market_order_if_resolvable(state);
}

void ShSzFullOrderBookManager::execute_if_present(InstrumentState* state, long quote_tag, int volume) {
    if (quote_tag <= 0 || volume <= 0) {
        return;
    }
    state->engine.execute_order(quote_tag, volume);
}

void ShSzFullOrderBookManager::cancel_if_present(InstrumentState* state, long quote_tag, int volume) {
    if (quote_tag <= 0 || volume <= 0) {
        return;
    }
    state->engine.cancel_order(quote_tag, volume);
}

void ShSzFullOrderBookManager::materialize_pending_market_order(InstrumentState* state, int price) {
    if (!state->pending_market_order.active || price <= 0) {
        return;
    }
    if (!state->engine.has_order(state->pending_market_order.quote_tag)) {
        state->engine.add_order(state->pending_market_order.quote_tag,
                                state->pending_market_order.is_sell,
                                price,
                                state->pending_market_order.volume,
                                state->pending_market_order.create_time_ms);
    }
    state->pending_market_order.active = false;
    state->pending_market_order.resolved_price = price;
}

void ShSzFullOrderBookManager::try_resolve_pending_market_order(InstrumentState* state, const LFL2TradeField* trade_data) {
    if (!state->pending_market_order.active) {
        return;
    }

    const bool matches_pending =
        (state->pending_market_order.is_sell && trade_data->OfferApplSeqNum == state->pending_market_order.quote_tag) ||
        (!state->pending_market_order.is_sell && trade_data->BidApplSeqNum == state->pending_market_order.quote_tag);
    if (!matches_pending) {
        return;
    }

    const int trade_price = round_price(trade_data->Price);
    materialize_pending_market_order(state, trade_price);
}

void ShSzFullOrderBookManager::flush_pending_market_order_if_resolvable(InstrumentState* state) {
    if (state == 0 || !state->pending_market_order.active) {
        return;
    }

    const int reference_price = pending_market_order_reference_price(state);
    materialize_pending_market_order(state, reference_price);
}

ShSzFullOrderBookManager::PendingMarketOrderSnapshot ShSzFullOrderBookManager::snapshot_pending_market_order(
    const PendingMarketOrder& pending_market_order) {
    PendingMarketOrderSnapshot snapshot;
    snapshot.active = pending_market_order.active;
    snapshot.is_sell = pending_market_order.is_sell;
    snapshot.quote_tag = pending_market_order.quote_tag;
    snapshot.volume = pending_market_order.volume;
    snapshot.create_time_ms = pending_market_order.create_time_ms;
    snapshot.resolved_price = pending_market_order.resolved_price;
    return snapshot;
}

int ShSzFullOrderBookManager::pending_market_order_reference_price(const InstrumentState* state) {
    if (state == 0 || !state->pending_market_order.active) {
        return 0;
    }
    return state->pending_market_order.is_sell ? state->engine.best_bid_price() : state->engine.best_ask_price();
}

bool ShSzFullOrderBookManager::is_auction_time(const char* time_text) {
    return time_text != 0 &&
           ((std::strcmp(time_text, "09:26:00.000") < 0) ||
            (std::strcmp(time_text, "14:57:00.000") >= 0));
}

int ShSzFullOrderBookManager::round_price(double price) {
    return static_cast<int>(std::round(price * PRICE_MULTIPLIER));
}

double ShSzFullOrderBookManager::parse_instrument_id_value(const char* instrument_id) {
    double value = 0.0;
    if (instrument_id == 0) {
        return value;
    }
    for (int i = 0; i < 6 && instrument_id[i] != '\0'; ++i) {
        value = value * 10.0 + static_cast<double>(instrument_id[i] - '0');
    }
    return value;
}
