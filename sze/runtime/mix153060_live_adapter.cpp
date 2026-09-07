#include "sze/runtime/mix153060_live_adapter.h"

#include <cmath>
#include <limits>

namespace mix153060 {

namespace {

void set_error(std::string* error, const char* value) {
    if (error != 0) {
        *error = value;
    }
}

bool valid_common(int64_t sequence,
                  double price,
                  double volume,
                  const char* time,
                  int32_t trading_date,
                  int64_t receive_time,
                  int64_t* exchange_time_us,
                  int64_t* local_time_us,
                  std::string* error) {
    if (sequence <= 0) {
        set_error(error, "invalid application sequence");
        return false;
    }
    if (!std::isfinite(price)) {
        set_error(error, "invalid price");
        return false;
    }
    const double max_price =
        static_cast<double>(std::numeric_limits<int>::max()) / 100.0;
    if (price < 0.0 || price > max_price) {
        set_error(error, "price is outside native tick range");
        return false;
    }
    if (!std::isfinite(volume) || volume <= 0.0 ||
        volume > static_cast<double>(std::numeric_limits<int64_t>::max()) ||
        std::floor(volume) != volume) {
        set_error(error, "invalid volume");
        return false;
    }
    if (!parse_exchange_time_us(time, trading_date, exchange_time_us)) {
        set_error(error, "invalid exchange time");
        return false;
    }
    *local_time_us = normalize_receive_time_us(
        receive_time, trading_date, *exchange_time_us);
    return true;
}

}  // namespace

bool normalize_order_event(const LFL2OrderField& source,
                           int32_t trading_date,
                           int64_t receive_time,
                           OrderEvent* destination,
                           std::string* error) {
    if (error != 0) {
        error->clear();
    }
    if (destination == 0) {
        set_error(error, "null order destination");
        return false;
    }
    *destination = OrderEvent();
    if (!valid_common(source.ApplSeqNum, source.Price, source.Volume,
                      source.OrderTime, trading_date, receive_time,
                      &destination->exchange_time_us,
                      &destination->local_time_us, error)) {
        return false;
    }
    destination->app_sequence = source.ApplSeqNum;
    destination->price = source.Price;
    destination->volume = static_cast<int64_t>(source.Volume);
    if (source.OrderKind[0] == 'B') {
        destination->buy = true;
    } else if (source.OrderKind[0] == 'S') {
        destination->buy = false;
    } else {
        set_error(error, "unsupported order side");
        return false;
    }
    if (source.OrdType[0] == '1') {
        destination->kind = OrderKind::kMarket;
    } else if (source.OrdType[0] == 'U') {
        destination->kind = OrderKind::kSelfBest;
    } else if (source.OrdType[0] == '2') {
        destination->kind = OrderKind::kLimit;
    } else {
        set_error(error, "unsupported order type");
        return false;
    }
    if (destination->kind == OrderKind::kLimit && source.Price <= 0.0) {
        set_error(error, "invalid order price");
        return false;
    }
    return true;
}

bool normalize_trade_event(const LFL2TradeField& source,
                           int32_t trading_date,
                           int64_t receive_time,
                           TradeEvent* destination,
                           std::string* error) {
    if (error != 0) {
        error->clear();
    }
    if (destination == 0) {
        set_error(error, "null trade destination");
        return false;
    }
    *destination = TradeEvent();
    if (!valid_common(source.ApplSeqNum, source.Price, source.Volume,
                      source.TradeTime, trading_date, receive_time,
                      &destination->exchange_time_us,
                      &destination->local_time_us, error)) {
        return false;
    }
    destination->app_sequence = source.ApplSeqNum;
    destination->price = source.Price;
    destination->volume = static_cast<int64_t>(source.Volume);
    destination->buy_order_id = source.BidApplSeqNum;
    destination->sell_order_id = source.OfferApplSeqNum;
    if (source.OrderKind[0] == 'F') {
        destination->kind = TradeKind::kFill;
    } else if (source.OrderKind[0] == '4') {
        destination->kind = TradeKind::kCancel;
    } else {
        set_error(error, "unsupported trade type");
        return false;
    }
    if (source.BidApplSeqNum < 0 || source.OfferApplSeqNum < 0) {
        set_error(error, "invalid trade order id");
        return false;
    }
    if (destination->kind == TradeKind::kFill) {
        if (source.Price <= 0.0 || source.BidApplSeqNum == 0 ||
            source.OfferApplSeqNum == 0 ||
            source.BidApplSeqNum == source.OfferApplSeqNum) {
            set_error(error, "invalid fill fields");
            return false;
        }
    } else if (source.BidApplSeqNum == 0 && source.OfferApplSeqNum == 0) {
        set_error(error, "cancel has no order id");
        return false;
    }
    return true;
}

}  // namespace mix153060
