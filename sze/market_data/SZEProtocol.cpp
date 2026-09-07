#include "sze/market_data/SZEProtocol.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace sze_md {
namespace {

const std::uint8_t kSzeExchangeId = 101;
const std::uint64_t kMaxInt64 =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

bool valid_date_time(std::uint64_t value) {
    // YYYYMMDDHHMMSSmmm. The protocol deliberately carries the date so a
    // reconnect cannot silently turn a previous session into today's events.
    if (value < 10000000000000000ULL || value > 99999999999999999ULL) {
        return false;
    }
    const std::uint64_t tod = value % 1000000000ULL;
    const unsigned hour = static_cast<unsigned>(tod / 10000000ULL);
    const unsigned minute = static_cast<unsigned>((tod / 100000ULL) % 100ULL);
    const unsigned second = static_cast<unsigned>((tod / 1000ULL) % 100ULL);
    const unsigned millis = static_cast<unsigned>(tod % 1000ULL);
    if (hour >= 24U || minute >= 60U || second > 60U || millis >= 1000U) {
        return false;
    }

    const unsigned date = static_cast<unsigned>(value / 1000000000ULL);
    const unsigned year = date / 10000U;
    const unsigned month = (date / 100U) % 100U;
    const unsigned day = date % 100U;
    if (year < 1970U || year > 9999U || month < 1U || month > 12U || day < 1U) {
        return false;
    }
    static const unsigned days_by_month[12] =
        {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    unsigned max_day = days_by_month[month - 1U];
    const bool leap = (year % 4U == 0U) &&
                      ((year % 100U != 0U) || (year % 400U == 0U));
    if (month == 2U && leap) {
        max_day = 29U;
    }
    return day <= max_day;
}

bool copy_symbol(const std::uint8_t* source, char* destination, std::size_t size) {
    if (source == 0 || destination == 0 || size < 2) {
        return false;
    }
    std::size_t length = 0;
    while (length < 9U && source[length] != 0 && source[length] != ' ') {
        const unsigned char c = source[length];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) {
            return false;
        }
        ++length;
    }
    if (length == 0 || length >= size) {
        return false;
    }
    std::memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

bool format_exchange_time(std::uint64_t value, char* destination, std::size_t size) {
    if (!valid_date_time(value) || destination == 0 || size < 13) {
        return false;
    }
    const std::uint64_t tod = value % 1000000000ULL;
    const unsigned hour = static_cast<unsigned>(tod / 10000000ULL);
    const unsigned minute = static_cast<unsigned>((tod / 100000ULL) % 100ULL);
    const unsigned second = static_cast<unsigned>((tod / 1000ULL) % 100ULL);
    const unsigned millis = static_cast<unsigned>(tod % 1000ULL);
    destination[0] = static_cast<char>('0' + hour / 10U);
    destination[1] = static_cast<char>('0' + hour % 10U);
    destination[2] = ':';
    destination[3] = static_cast<char>('0' + minute / 10U);
    destination[4] = static_cast<char>('0' + minute % 10U);
    destination[5] = ':';
    destination[6] = static_cast<char>('0' + second / 10U);
    destination[7] = static_cast<char>('0' + second % 10U);
    destination[8] = '.';
    destination[9] = static_cast<char>('0' + millis / 100U);
    destination[10] = static_cast<char>('0' + (millis / 10U) % 10U);
    destination[11] = static_cast<char>('0' + millis % 10U);
    destination[12] = '\0';
    return true;
}

void copy_common(const SzeHpfHead& head,
                 char* exchange,
                 std::int64_t* application_sequence,
                 std::int64_t* business_sequence) {
    std::memcpy(exchange, "SZE", 4U);
    *application_sequence = static_cast<std::int64_t>(head.sequence_num);
    *business_sequence = static_cast<std::int64_t>(head.sequence);
}

void set_failure(DecodeFailureReason* destination, DecodeFailureReason reason) {
    if (destination != 0) {
        *destination = reason;
    }
}

}  // namespace

bool decode_snapshot(const void* record, std::size_t length,
                     LFMarketDataField* snapshot) {
    if (record == 0 || snapshot == 0 || length != kSnapshotRecordSize) {
        return false;
    }
    const SzeHpfSnapshot* source = static_cast<const SzeHpfSnapshot*>(record);
    if (source->head.message_type != kSnapshotMessage ||
        source->head.exchange_id != kSzeExchangeId ||
        source->head.sequence_num == 0 ||
        !copy_symbol(source->head.symbol, snapshot->InstrumentID,
                     sizeof(snapshot->InstrumentID)) ||
        !valid_date_time(source->head.quote_update_time)) {
        return false;
    }

    std::memset(snapshot, 0, sizeof(*snapshot));
    if (!copy_symbol(source->head.symbol, snapshot->InstrumentID,
                     sizeof(snapshot->InstrumentID))) {
        return false;
    }
    std::memcpy(snapshot->ExchangeID, "SZE", 4U);
    std::memcpy(snapshot->ExchangeInstID, snapshot->InstrumentID,
                std::strlen(snapshot->InstrumentID));
    const std::uint64_t date = source->head.quote_update_time / 1000000000ULL;
    std::snprintf(snapshot->TradingDay, sizeof(snapshot->TradingDay), "%08llu",
                  static_cast<unsigned long long>(date));
    if (!format_exchange_time(source->head.quote_update_time,
                              snapshot->UpdateTime, sizeof(snapshot->UpdateTime))) {
        return false;
    }
    snapshot->UpdateMillisec = static_cast<int>(source->head.quote_update_time % 1000ULL);
    snapshot->LastPrice = source->last_price / 10000.0;
    snapshot->PreClosePrice = source->pre_close_price / 10000.0;
    snapshot->OpenPrice = source->open_price / 10000.0;
    snapshot->HighestPrice = source->day_high_price / 10000.0;
    snapshot->LowestPrice = source->day_low_price / 10000.0;
    snapshot->ClosePrice = source->today_close_price / 10000.0;
    snapshot->UpperLimitPrice = source->upper_limit_price / 10000.0;
    snapshot->LowerLimitPrice = source->low_limit_price / 10000.0;
    snapshot->Volume = static_cast<int>(std::min<std::uint64_t>(
        source->total_quantity / 100ULL,
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
    snapshot->Turnover = source->total_value / 1000000.0;
    snapshot->OpenInterest = source->open_interest / 10000.0;

    double* bid_prices[] = {&snapshot->BidPrice1, &snapshot->BidPrice2,
        &snapshot->BidPrice3, &snapshot->BidPrice4, &snapshot->BidPrice5,
        &snapshot->BidPrice6, &snapshot->BidPrice7, &snapshot->BidPrice8,
        &snapshot->BidPrice9, &snapshot->BidPriceA};
    int* bid_volumes[] = {&snapshot->BidVolume1, &snapshot->BidVolume2,
        &snapshot->BidVolume3, &snapshot->BidVolume4, &snapshot->BidVolume5,
        &snapshot->BidVolume6, &snapshot->BidVolume7, &snapshot->BidVolume8,
        &snapshot->BidVolume9, &snapshot->BidVolumeA};
    double* ask_prices[] = {&snapshot->AskPrice1, &snapshot->AskPrice2,
        &snapshot->AskPrice3, &snapshot->AskPrice4, &snapshot->AskPrice5,
        &snapshot->AskPrice6, &snapshot->AskPrice7, &snapshot->AskPrice8,
        &snapshot->AskPrice9, &snapshot->AskPriceA};
    int* ask_volumes[] = {&snapshot->AskVolume1, &snapshot->AskVolume2,
        &snapshot->AskVolume3, &snapshot->AskVolume4, &snapshot->AskVolume5,
        &snapshot->AskVolume6, &snapshot->AskVolume7, &snapshot->AskVolume8,
        &snapshot->AskVolume9, &snapshot->AskVolumeA};
    for (std::size_t index = 0; index < 10U; ++index) {
        const int bid_volume = static_cast<int>(std::min<std::uint64_t>(
            source->bid[index].quantity / 100ULL,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
        const int ask_volume = static_cast<int>(std::min<std::uint64_t>(
            source->ask[index].quantity / 100ULL,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
        const double bid_price = source->bid[index].price / 10000.0;
        const double ask_price = source->ask[index].price / 10000.0;
        *bid_prices[index] = snapshot->aBidPrice[index] = bid_price;
        *bid_volumes[index] = snapshot->aBidVolume[index] = bid_volume;
        *ask_prices[index] = snapshot->aAskPrice[index] = ask_price;
        *ask_volumes[index] = snapshot->aAskVolume[index] = ask_volume;
    }
    return true;
}

bool is_official_heartbeat(std::uint8_t message_type) {
    switch (message_type) {
        case kSnapshotHeartbeatMessage:
        case kIndexHeartbeatMessage:
        case kTickHeartbeatMessage:
        case kAfterCloseHeartbeatMessage:
        case kTurnoverHeartbeatMessage:
        case kIbrTreeHeartbeatMessage:
        case kTreeHeartbeatMessage:
        case kBondSnapshotHeartbeatMessage:
        case kBondTickHeartbeatMessage:
            return true;
        default:
            return false;
    }
}

std::size_t wire_record_size(std::uint8_t message_type) {
    if (is_official_heartbeat(message_type)) {
        return kHeartbeatRecordSize;
    }
    switch (message_type) {
        case kSnapshotMessage: return kSnapshotRecordSize;
        case kIndexMessage: return kIndexRecordSize;
        case kOrderMessage: return kOrderRecordSize;
        case kExecutionMessage: return kExecutionRecordSize;
        case kAfterCloseMessage: return kAfterCloseRecordSize;
        case kTurnoverMessage: return kTurnoverRecordSize;
        case kIbrTreeMessage: return kIbrTreeRecordSize;
        case kTreeMessage: return kTreeRecordSize;
        case kBondSnapshotMessage: return kBondSnapshotRecordSize;
        case kBondOrderMessage: return kBondOrderRecordSize;
        case kBondExecutionMessage: return kBondExecutionRecordSize;
        default: return 0U;
    }
}

DecodeStatus decode_record(const void* record,
                           std::size_t length,
                           LFL2OrderField* order,
                           LFL2TradeField* trade,
                           DecodeFailureReason* failure) {
    set_failure(failure, DecodeFailureReason::kNone);
    if (record == 0) {
        set_failure(failure, DecodeFailureReason::kNullRecord);
        return DecodeStatus::kMalformed;
    }
    if (length < 9U) {
        set_failure(failure, DecodeFailureReason::kInvalidLength);
        return DecodeStatus::kMalformed;
    }

    const std::uint8_t message_type =
        *(static_cast<const std::uint8_t*>(record) + 8U);
    const std::size_t expected_size = wire_record_size(message_type);
    if (expected_size == 0U) {
        set_failure(failure,
                    length == sizeof(SzeHpfHeartbeat)
                        ? DecodeFailureReason::kUnexpectedHeartbeatType
                        : DecodeFailureReason::kUnsupportedMessageType);
        return DecodeStatus::kUnknown;
    }
    if (length != expected_size) {
        set_failure(failure, DecodeFailureReason::kInvalidLength);
        return DecodeStatus::kMalformed;
    }
    if (is_official_heartbeat(message_type)) {
        return DecodeStatus::kHeartbeat;
    }
    if (message_type != kOrderMessage && message_type != kExecutionMessage) {
        return DecodeStatus::kKnownNonTarget;
    }

    if (message_type == kOrderMessage) {
        if (order == 0) {
            set_failure(failure, DecodeFailureReason::kMissingOrderOutput);
            return DecodeStatus::kMalformed;
        }
        const SzeHpfOrder* source = static_cast<const SzeHpfOrder*>(record);
        char symbol[sizeof(order->InstrumentID)] = {};
        char order_time[sizeof(order->OrderTime)] = {};
        if (source->head.exchange_id != kSzeExchangeId) {
            set_failure(failure, DecodeFailureReason::kInvalidOrderExchange);
            return DecodeStatus::kMalformed;
        }
        if (source->head.sequence_num == 0 ||
            source->head.sequence_num > kMaxInt64) {
            set_failure(failure, DecodeFailureReason::kInvalidOrderSequence);
            return DecodeStatus::kMalformed;
        }
        if (source->side_flag != '1' && source->side_flag != '2') {
            set_failure(failure, DecodeFailureReason::kInvalidOrderSide);
            return DecodeStatus::kMalformed;
        }
        if (source->order_type != '1' && source->order_type != '2' &&
            source->order_type != 'U') {
            set_failure(failure, DecodeFailureReason::kInvalidOrderType);
            return DecodeStatus::kMalformed;
        }
        if (source->order_quantity == 0) {
            set_failure(failure, DecodeFailureReason::kInvalidOrderQuantity);
            return DecodeStatus::kMalformed;
        }
        if (!copy_symbol(source->head.symbol, symbol, sizeof(symbol))) {
            set_failure(failure, DecodeFailureReason::kInvalidOrderSymbol);
            return DecodeStatus::kMalformed;
        }
        if (!format_exchange_time(source->head.quote_update_time,
                                  order_time, sizeof(order_time))) {
            set_failure(failure, DecodeFailureReason::kInvalidOrderTime);
            return DecodeStatus::kMalformed;
        }
        std::memset(order, 0, sizeof(*order));
        std::memcpy(order->OrderTime, order_time, sizeof(order->OrderTime));
        std::memcpy(order->InstrumentID, symbol, sizeof(order->InstrumentID));
        copy_common(source->head, order->ExchangeID,
                    &order->ApplSeqNum, &order->BizIndex);
        order->Price = static_cast<double>(source->order_price) / 10000.0;
        order->Volume = static_cast<double>(source->order_quantity) / 100.0;
        order->OrderKind[0] = source->side_flag == '1' ? 'B' : 'S';
        order->OrdType[0] = source->order_type;
        order->OrderNo = 0;
        return DecodeStatus::kOrder;
    }

    if (message_type == kExecutionMessage) {
        if (trade == 0) {
            set_failure(failure, DecodeFailureReason::kMissingTradeOutput);
            return DecodeStatus::kMalformed;
        }
        const SzeHpfExecution* source =
            static_cast<const SzeHpfExecution*>(record);
        const bool is_fill = source->trade_type == 'F';
        const bool is_cancel = source->trade_type == '4';
        const bool ids_nonnegative = source->trade_buy_num >= 0 &&
                                     source->trade_sell_num >= 0;
        const bool fill_ids_valid = source->trade_buy_num > 0 &&
                                    source->trade_sell_num > 0 &&
                                    source->trade_buy_num != source->trade_sell_num;
        const bool cancel_id_valid = source->trade_buy_num > 0 ||
                                     source->trade_sell_num > 0;
        char symbol[sizeof(trade->InstrumentID)] = {};
        char trade_time[sizeof(trade->TradeTime)] = {};
        if (source->head.exchange_id != kSzeExchangeId) {
            set_failure(failure, DecodeFailureReason::kInvalidTradeExchange);
            return DecodeStatus::kMalformed;
        }
        if (source->head.sequence_num == 0 ||
            source->head.sequence_num > kMaxInt64) {
            set_failure(failure, DecodeFailureReason::kInvalidTradeSequence);
            return DecodeStatus::kMalformed;
        }
        if (!is_fill && !is_cancel) {
            set_failure(failure, DecodeFailureReason::kInvalidTradeType);
            return DecodeStatus::kMalformed;
        }
        if (!ids_nonnegative || (is_fill && !fill_ids_valid) ||
            (is_cancel && !cancel_id_valid)) {
            set_failure(failure, DecodeFailureReason::kInvalidTradeIds);
            return DecodeStatus::kMalformed;
        }
        if (source->trade_quantity <= 0) {
            set_failure(failure, DecodeFailureReason::kInvalidTradeQuantity);
            return DecodeStatus::kMalformed;
        }
        if (is_fill && source->trade_price == 0) {
            set_failure(failure, DecodeFailureReason::kInvalidTradePrice);
            return DecodeStatus::kMalformed;
        }
        if (!copy_symbol(source->head.symbol, symbol, sizeof(symbol))) {
            set_failure(failure, DecodeFailureReason::kInvalidTradeSymbol);
            return DecodeStatus::kMalformed;
        }
        if (!format_exchange_time(source->head.quote_update_time,
                                  trade_time, sizeof(trade_time))) {
            set_failure(failure, DecodeFailureReason::kInvalidTradeTime);
            return DecodeStatus::kMalformed;
        }
        std::memset(trade, 0, sizeof(*trade));
        std::memcpy(trade->TradeTime, trade_time, sizeof(trade->TradeTime));
        std::memcpy(trade->InstrumentID, symbol, sizeof(trade->InstrumentID));
        copy_common(source->head, trade->ExchangeID,
                    &trade->ApplSeqNum, &trade->BizIndex);
        trade->Price = static_cast<double>(source->trade_price) / 10000.0;
        trade->Volume = static_cast<double>(source->trade_quantity) / 100.0;
        trade->OrderKind[0] = source->trade_type;
        trade->BidApplSeqNum = source->trade_buy_num;
        trade->OfferApplSeqNum = source->trade_sell_num;
        trade->TurnOver = is_fill ? trade->Price * trade->Volume : 0.0;
        // SZE does not carry an aggressor flag. Preserve the repository's
        // established convention: the later (larger) application sequence is
        // the active side. For cancels the absent side is zero, so this also
        // identifies the cancelled side.
        trade->OrderBSFlag[0] =
            trade->BidApplSeqNum > trade->OfferApplSeqNum ? 'B' : 'S';
        return DecodeStatus::kExecution;
    }

    set_failure(failure, DecodeFailureReason::kUnsupportedMessageType);
    return DecodeStatus::kUnknown;
}

DecodeStatus decode_recovery_record(const sze_recovery::CanonicalEvent& event,
                                    const void* record,
                                    std::size_t length,
                                    LFL2OrderField* order,
                                    LFL2TradeField* trade,
                                    DecodeFailureReason* failure)
{
    set_failure(failure, DecodeFailureReason::kNone);
    if (record == 0 || event.record_kind != sze_recovery::kRecordMarketData ||
        event.source_id != 88U || event.payload_size != length ||
        event.payload_crc32 != sze_recovery::crc32(record, length) ||
        length != sizeof(SzeHpfOrder)) {
        set_failure(failure, DecodeFailureReason::kInvalidRecoveryEnvelope);
        return DecodeStatus::kMalformed;
    }
    SzeHpfHead head;
    std::memcpy(&head, record, sizeof(head));
    if (head.message_type != event.message_type ||
        head.sequence != static_cast<std::uint32_t>(
            event.feed_sequence & 0xffffffffULL) ||
        head.sequence_num != event.channel_sequence ||
        head.channel_num != event.channel_number ||
        head.quote_update_time != event.exchange_time ||
        head.quote_update_time / 1000000000ULL != event.trading_day) {
        set_failure(failure, DecodeFailureReason::kInvalidRecoveryEnvelope);
        return DecodeStatus::kMalformed;
    }
    return decode_record(record, length, order, trade, failure);
}

const char* decode_status_name(DecodeStatus status) {
    switch (status) {
        case DecodeStatus::kOrder: return "order";
        case DecodeStatus::kExecution: return "execution";
        case DecodeStatus::kHeartbeat: return "heartbeat";
        case DecodeStatus::kUnknown: return "unknown";
        case DecodeStatus::kMalformed: return "malformed";
        case DecodeStatus::kKnownNonTarget: return "known-non-target";
    }
    return "invalid";
}

const char* decode_failure_reason_name(DecodeFailureReason reason) {
    switch (reason) {
        case DecodeFailureReason::kNone: return "none";
        case DecodeFailureReason::kNullRecord: return "null-record";
        case DecodeFailureReason::kInvalidLength: return "invalid-length";
        case DecodeFailureReason::kUnexpectedHeartbeatType: return "unexpected-heartbeat-type";
        case DecodeFailureReason::kUnsupportedMessageType: return "unsupported-message-type";
        case DecodeFailureReason::kMissingOrderOutput: return "missing-order-output";
        case DecodeFailureReason::kInvalidOrderExchange: return "invalid-order-exchange";
        case DecodeFailureReason::kInvalidOrderSequence: return "invalid-order-sequence";
        case DecodeFailureReason::kInvalidOrderSide: return "invalid-order-side";
        case DecodeFailureReason::kInvalidOrderType: return "invalid-order-type";
        case DecodeFailureReason::kInvalidOrderQuantity: return "invalid-order-quantity";
        case DecodeFailureReason::kInvalidOrderSymbol: return "invalid-order-symbol";
        case DecodeFailureReason::kInvalidOrderTime: return "invalid-order-time";
        case DecodeFailureReason::kMissingTradeOutput: return "missing-trade-output";
        case DecodeFailureReason::kInvalidTradeExchange: return "invalid-trade-exchange";
        case DecodeFailureReason::kInvalidTradeSequence: return "invalid-trade-sequence";
        case DecodeFailureReason::kInvalidTradeType: return "invalid-trade-type";
        case DecodeFailureReason::kInvalidTradeIds: return "invalid-trade-ids";
        case DecodeFailureReason::kInvalidTradeQuantity: return "invalid-trade-quantity";
        case DecodeFailureReason::kInvalidTradePrice: return "invalid-trade-price";
        case DecodeFailureReason::kInvalidTradeSymbol: return "invalid-trade-symbol";
        case DecodeFailureReason::kInvalidTradeTime: return "invalid-trade-time";
        case DecodeFailureReason::kInvalidRecoveryEnvelope: return "invalid-recovery-envelope";
    }
    return "invalid";
}

}  // namespace sze_md
