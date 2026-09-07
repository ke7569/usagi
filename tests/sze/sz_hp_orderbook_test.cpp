#include "sze/market_data/sz_hp_orderbook.h"

#include <cstring>
#include <iostream>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

LFL2OrderField make_order(const char* time,
                          const char* instrument,
                          char side,
                          char type,
                          double price,
                          double volume,
                          int64_t sequence) {
    LFL2OrderField value;
    std::memset(&value, 0, sizeof(value));
    std::strncpy(value.OrderTime, time, sizeof(value.OrderTime) - 1);
    std::strncpy(value.InstrumentID, instrument, sizeof(value.InstrumentID) - 1);
    value.OrderKind[0] = side;
    value.OrdType[0] = type;
    value.Price = price;
    value.Volume = volume;
    value.ApplSeqNum = sequence;
    value.BizIndex = sequence + 1000;
    return value;
}

LFL2TradeField make_trade(const char* time,
                          const char* instrument,
                          char flag,
                          double price,
                          double volume,
                          int64_t sequence,
                          int64_t bid,
                          int64_t ask) {
    LFL2TradeField value;
    std::memset(&value, 0, sizeof(value));
    std::strncpy(value.TradeTime, time, sizeof(value.TradeTime) - 1);
    std::strncpy(value.InstrumentID, instrument, sizeof(value.InstrumentID) - 1);
    value.OrderKind[0] = flag;
    value.Price = price;
    value.Volume = volume;
    value.ApplSeqNum = sequence;
    value.BidApplSeqNum = bid;
    value.OfferApplSeqNum = ask;
    return value;
}

bool test_adapter() {
    const LFL2OrderField source = make_order(
        "09:30:00.123", "000001.SZ", 'B', 'U', 10.123, 100, 11);
    sz_hp::OrderEvent order;
    sz_hp::AdapterDiagnostic diagnostic;
    if (!check(sz_hp::EventAdapter::normalize_order(source, &order, &diagnostic, 77),
               "order adapter accepts valid event")) {
        return false;
    }
    if (!check(order.price == 10123 && order.event_time_ms == 34200123 &&
                   order.type == sz_hp::OrderType::kSelfBest && order.order_id == 11,
               "order adapter preserves HP fields")) {
        return false;
    }
    if (!check(diagnostic.code == sz_hp::AdapterDiagnostic::kNone &&
                   diagnostic.event_index == 77 && diagnostic.instrument[0] == '0',
               "successful adapter carries context without per-event logging")) {
        return false;
    }

    const char order_types[] = {'1', '2', 'A', 'X'};
    const sz_hp::OrderType expected_types[] = {
        sz_hp::OrderType::kMarketPrice,
        sz_hp::OrderType::kLimitPrice,
        sz_hp::OrderType::kNoop,
        sz_hp::OrderType::kNoop};
    for (size_t i = 0; i < sizeof(order_types) / sizeof(order_types[0]); ++i) {
        const LFL2OrderField typed_source = make_order(
            "23:59:59.999", "000001.SZ", i == 0 ? 'S' : 'B', order_types[i],
            0.0005, 1, static_cast<int64_t>(20 + i));
        sz_hp::OrderEvent typed_order;
        if (!check(sz_hp::EventAdapter::normalize_order(typed_source, &typed_order,
                                                        &diagnostic, 80 + i),
                   "all HP order flags normalize")) {
            return false;
        }
        if (!check(typed_order.type == expected_types[i] &&
                       typed_order.event_time_ms == 86399999 &&
                       typed_order.is_buy == (i != 0),
                   "order flag, side, and boundary time are preserved")) {
            return false;
        }
    }

    const LFL2TradeField trade_source = make_trade(
        "09:30:01.004", "000001.SZ", '4', 10.123, 20, 12, 11, 7);
    sz_hp::TradeEvent trade;
    if (!check(sz_hp::EventAdapter::normalize_trade(trade_source, &trade, &diagnostic),
               "trade adapter accepts valid event")) {
        return false;
    }
    if (!check(trade.flag == sz_hp::TradeFlag::kCancel && trade.price == 10123 &&
                   trade.bid_id == 11 && trade.ask_id == 7,
               "trade adapter preserves cancellation IDs")) {
        return false;
    }
    const LFL2TradeField fill_source = make_trade(
        "00:00:00.000", "000001.SZ", 'F', 10.0, 1, 13, 11, 7);
    if (!check(sz_hp::EventAdapter::normalize_trade(fill_source, &trade, &diagnostic, 91) &&
                   trade.flag == sz_hp::TradeFlag::kFill && trade.event_time_ms == 0,
               "fill trade maps to HP fill")) {
        return false;
    }
    const LFL2TradeField noop_source = make_trade(
        "09:30:01.004", "000001.SZ", 'X', 10.0, 1, 14, 11, 7);
    if (!check(sz_hp::EventAdapter::normalize_trade(noop_source, &trade, &diagnostic) &&
                   trade.flag == sz_hp::TradeFlag::kNoop,
               "unknown trade flag is an HP no-op")) {
        return false;
    }
    LFL2OrderField malformed = source;
    malformed.ApplSeqNum = 0;
    if (!check(!sz_hp::EventAdapter::normalize_order(malformed, &order, &diagnostic, 99) &&
                   diagnostic.code == sz_hp::AdapterDiagnostic::kInvalidSequence &&
                   diagnostic.event_index == 99 && diagnostic.instrument[0] == '0',
               "malformed sequence returns structured diagnostic")) {
        return false;
    }
    malformed = source;
    std::strncpy(malformed.OrderTime, "24:00:00", sizeof(malformed.OrderTime) - 1);
    if (!check(!sz_hp::EventAdapter::normalize_order(malformed, &order, &diagnostic) &&
                   diagnostic.code == sz_hp::AdapterDiagnostic::kInvalidTime,
               "out-of-range timestamp is rejected")) {
        return false;
    }

    LFL2MarketDataField market;
    std::memset(&market, 0, sizeof(market));
    std::strncpy(market.TimeStamp, "13:00:00.001", sizeof(market.TimeStamp) - 1);
    std::strncpy(market.InstrumentID, "000001.SZ", sizeof(market.InstrumentID) - 1);
    market.LastPrice = 10.0;
    market.TotalTradeVolume = 1000;
    market.TotalTradeValue = 10000;
    market.BidPrice1 = 9.99;
    market.BidVolume1 = 100;
    market.OfferPrice1 = 10.01;
    market.OfferVolume1 = 200;
    market.UpperLimitPrice = 11.0;
    market.LowerLimitPrice = 9.0;
    sz_hp::MarketObservation observation;
    if (!check(sz_hp::EventAdapter::normalize_observation(
                   market, &observation, &diagnostic, 101) && observation.valid &&
                   observation.event_time_ms == 46800001 && observation.fast_mid_price() == 10.0 &&
                   observation.upper_limit_price == 11.0 &&
                   observation.lower_limit_price == 9.0 &&
                   diagnostic.event_index == 101,
               "exchange observation maps at the same adapter boundary")) {
        return false;
    }
    market.OfferVolume1 = 0;
    if (!check(sz_hp::EventAdapter::normalize_observation(market, &observation, &diagnostic) &&
                   !observation.valid,
               "incomplete exchange observation remains an explicit invalid sample")) {
        return false;
    }
    return true;
}

bool test_window() {
    sz_hp::SlidingWindow window;
    if (!check(window.add(34200000, 10), "window add")) {
        return false;
    }
    if (!check(window.total(34230000) == 10, "window is closed at 30 seconds")) {
        return false;
    }
    if (!check(window.total(34231000) == 0, "window expires after 30 seconds")) {
        return false;
    }
    if (!check(window.add(34232000, 5) && window.erase(34232000, 5) &&
                   window.total(34232000) == 0,
               "window erase removes current bucket")) {
        return false;
    }
    if (!check(window.add(34200000, 5) && window.total(34232000) == 0,
               "old event is ignored after the window advances")) {
        return false;
    }
    if (!check(!window.erase(34232000, 1), "window detects underflow")) {
        return false;
    }
    if (!check(window.add(34232000 + 32U * 1000U, 9) &&
                   window.total(34232000 + 32U * 1000U) == 9,
               "large gaps clear expired buckets")) {
        return false;
    }
    return true;
}

bool test_book_lifecycle() {
    sz_hp::OrderBook book("000001.SZ");
    if (!check(book.add_order(1, true, 10000, 100, 34200000), "add bid")) {
        return false;
    }
    if (!check(book.add_order(2, false, 10100, 100, 34200001), "add ask")) {
        return false;
    }
    if (!check(book.best_bid_price() == 10000 && book.best_ask_price() == 10100,
               "best prices use HP side ordering")) {
        return false;
    }

    if (!check(book.add_order(3, true, 9900, 50, 34200002, sz_hp::OrderType::kSelfBest),
               "self-best joins bid level one")) {
        return false;
    }
    if (!check(book.best_bid_volume() == 150, "self-best volume is level-one volume")) {
        return false;
    }

    sz_hp::OrderBook duplicate_book("000001.SZ");
    if (!check(duplicate_book.add_order(9, true, 10000, 1, 34200003) &&
                   !duplicate_book.add_order(9, true, 10000, 1, 34200004) &&
                   !duplicate_book.available(),
               "duplicate order ID freezes the instrument")) {
        return false;
    }

    sz_hp::TradeEvent partial;
    partial.sequence = 4;
    partial.bid_id = 1;
    partial.ask_id = 2;
    partial.quantity = 40;
    partial.price = 10100;
    partial.flag = sz_hp::TradeFlag::kFill;
    if (!check(book.update_trade(partial), "partial fill")) {
        return false;
    }
    if (!check(book.best_bid_volume() == 110 && book.best_ask_volume() == 60,
               "partial fill updates both sides")) {
        return false;
    }

    partial.sequence = 5;
    partial.quantity = 60;
    if (!check(book.update_trade(partial), "full fill")) {
        return false;
    }
    if (!check(book.best_ask_price() == 0 && book.best_bid_volume() == 50,
               "empty level is removed after full fill")) {
        return false;
    }

    if (!check(book.add_order(4, false, 10200, 30, 34200003), "add second ask")) {
        return false;
    }
    if (!check(book.cancel_order(4, 9999, false, 6, 34200004),
               "cancel fallback scan")) {
        return false;
    }
    if (!check(book.best_ask_price() == 0, "fallback removed ask")) {
        return false;
    }

    sz_hp::TradeEvent invalid;
    invalid.sequence = 7;
    invalid.bid_id = 999;
    invalid.ask_id = 998;
    invalid.quantity = 1;
    invalid.flag = sz_hp::TradeFlag::kFill;
    if (!check(!book.update_trade(invalid) && !book.available(),
               "missing level-one order fails closed")) {
        return false;
    }
    const std::string frozen_digest = book.digest();
    if (!check(book.add_order(8, true, 9900, 1, 34200005) &&
                   book.digest() == frozen_digest,
               "unavailable book remains frozen")) {
        return false;
    }
    return true;
}

bool test_order_types_and_reset() {
    sz_hp::OrderBook book("000002.SZ");
    sz_hp::OrderEvent self_best;
    self_best.sequence = 1;
    self_best.order_id = 1;
    self_best.event_time_ms = 34200000;
    self_best.price = 9900;
    self_best.quantity = 10;
    self_best.is_buy = true;
    self_best.type = sz_hp::OrderType::kSelfBest;
    if (!check(book.update_order(self_best) && book.best_bid_price() == 9900,
               "self-best uses own price when its side is empty")) {
        return false;
    }
    if (!check(book.add_order(2, true, 10000, 20, 34200001) &&
                   book.add_order(3, false, 10100, 30, 34200002) &&
                   book.add_order(4, false, 10200, 40, 34200003),
               "ordered depth accepts multiple levels")) {
        return false;
    }
    if (!check(book.bids().first_level()->price() == 10000 &&
                   book.bids().last_level()->price() == 9900 &&
                   book.asks().first_level()->price() == 10100 &&
                   book.asks().last_level()->price() == 10200,
               "first and last traversal match HP ordering")) {
        return false;
    }
    const std::string digest = book.digest();
    if (!check(digest.find("bid=10000") != std::string::npos &&
                   digest.find(":1:20[") != std::string::npos,
               "digest includes ordered levels and rolling-window totals")) {
        return false;
    }

    sz_hp::OrderBook market_book("000003.SZ");
    if (!check(market_book.add_order(1, false, 0, 10, 34200000,
                                     sz_hp::OrderType::kMarketPrice) &&
                   market_book.best_ask_price() == 0,
               "market-price order uses the converted feed price")) {
        return false;
    }
    market_book.clear();
    if (!check(market_book.available() && market_book.order_count() == 0 &&
                   market_book.failure_sequence() == 0,
               "session reset clears state and availability")) {
        return false;
    }
    return true;
}

bool test_failure_transitions() {
    sz_hp::OrderBook missing_order("000004.SZ");
    if (!check(missing_order.add_order(1, true, 10000, 10, 34200000) &&
                   missing_order.add_order(2, false, 10100, 10, 34200001),
               "failure fixture setup")) {
        return false;
    }
    sz_hp::TradeEvent fill;
    fill.sequence = 3;
    fill.bid_id = 99;
    fill.ask_id = 2;
    fill.quantity = 1;
    fill.flag = sz_hp::TradeFlag::kFill;
    if (!check(!missing_order.update_trade(fill) && !missing_order.available() &&
                   missing_order.failure_reason() == "bid level one fill failed",
               "missing level-one order freezes the book")) {
        return false;
    }

    sz_hp::OrderBook cancel_book("000005.SZ");
    if (!check(cancel_book.add_order(10, true, 10000, 10, 34200000) &&
                   cancel_book.add_order(20, false, 10100, 10, 34200001),
               "cancel fixture setup")) {
        return false;
    }
    sz_hp::TradeEvent cancel;
    cancel.sequence = 21;
    cancel.bid_id = 10;
    cancel.ask_id = 1;
    cancel.price = 9999;
    cancel.quantity = 1;
    cancel.flag = sz_hp::TradeFlag::kCancel;
    if (!check(cancel_book.update_trade(cancel) && cancel_book.best_bid_price() == 0,
               "larger bid ID selects the buy-side cancel")) {
        return false;
    }
    cancel.sequence = 22;
    cancel.bid_id = 1;
    cancel.ask_id = 20;
    cancel.price = 9999;
    if (!check(cancel_book.update_trade(cancel) && cancel_book.best_ask_price() == 0,
               "larger ask ID selects the sell-side cancel")) {
        return false;
    }
    cancel_book.clear();
    cancel.sequence = 23;
    cancel.bid_id = 30;
    cancel.ask_id = 1;
    if (!check(!cancel_book.update_trade(cancel) && !cancel_book.available(),
               "missing cancellation target freezes the book")) {
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!test_adapter() || !test_window() || !test_book_lifecycle() ||
        !test_order_types_and_reset() || !test_failure_transitions()) {
        return 1;
    }
    std::cout << "sz_hp_orderbook_test: PASS\n";
    return 0;
}
