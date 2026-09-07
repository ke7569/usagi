#include "sze/market_data/SZEProtocol.h"

#include <cassert>
#include <cmath>
#include <cstring>

namespace {

void set_symbol(std::uint8_t* destination, const char* value)
{
    std::memset(destination, 0, 9);
    std::memcpy(destination, value, std::strlen(value));
}

sze_md::SzeHpfHead make_head(std::uint8_t type, std::uint64_t sequence)
{
    sze_md::SzeHpfHead head{};
    head.sequence = 77;
    head.message_type = type;
    head.security_type = 1;
    head.sub_security_type = 0;
    set_symbol(head.symbol, "000001");
    head.exchange_id = 101;
    head.quote_update_time = 20260720143015123ULL;
    head.channel_num = 2011;
    head.sequence_num = sequence;
    head.md_stream_id = 1;
    return head;
}

void test_order()
{
    sze_md::SzeHpfOrder wire{};
    wire.head = make_head(sze_md::kOrderMessage, 1001);
    wire.order_price = 123450;
    wire.order_quantity = 2500;
    wire.side_flag = '1';
    wire.order_type = '2';

    LFL2OrderField result{};
    assert(sze_md::decode_record(&wire, sizeof(wire), &result, 0) ==
           sze_md::DecodeStatus::kOrder);
    assert(std::strcmp(result.OrderTime, "14:30:15.123") == 0);
    assert(std::strcmp(result.ExchangeID, "SZE") == 0);
    assert(std::strcmp(result.InstrumentID, "000001") == 0);
    assert(result.ApplSeqNum == 1001);
    assert(result.BizIndex == 77);
    assert(result.OrderKind[0] == 'B');
    assert(result.OrdType[0] == '2');
    assert(std::fabs(result.Price - 12.345) < 1e-12);
    assert(std::fabs(result.Volume - 25.0) < 1e-12);
}

void test_fill_and_cancel()
{
    sze_md::SzeHpfExecution fill{};
    fill.head = make_head(sze_md::kExecutionMessage, 2001);
    fill.trade_buy_num = 1001;
    fill.trade_sell_num = 1002;
    fill.trade_price = 123460;
    fill.trade_quantity = 1000;
    fill.trade_type = 'F';

    LFL2TradeField result{};
    assert(sze_md::decode_record(&fill, sizeof(fill), 0, &result) ==
           sze_md::DecodeStatus::kExecution);
    assert(result.OrderKind[0] == 'F');
    assert(result.OrderBSFlag[0] == 'S');
    assert(result.BidApplSeqNum == 1001);
    assert(result.OfferApplSeqNum == 1002);
    assert(result.ApplSeqNum == 2001);
    assert(std::fabs(result.Price - 12.346) < 1e-12);
    assert(std::fabs(result.Volume - 10.0) < 1e-12);
    assert(std::fabs(result.TurnOver - 123.46) < 1e-10);

    sze_md::SzeHpfExecution cancel{};
    cancel.head = make_head(sze_md::kExecutionMessage, 2002);
    cancel.trade_buy_num = 1001;
    cancel.trade_sell_num = 0;
    cancel.trade_price = 0;
    cancel.trade_quantity = 500;
    cancel.trade_type = '4';
    std::memset(&result, 0, sizeof(result));
    assert(sze_md::decode_record(&cancel, sizeof(cancel), 0, &result) ==
           sze_md::DecodeStatus::kExecution);
    assert(result.OrderKind[0] == '4');
    assert(result.OrderBSFlag[0] == 'B');
    assert(result.BidApplSeqNum == 1001);
    assert(result.OfferApplSeqNum == 0);
    assert(result.TurnOver == 0.0);
}

void test_snapshot()
{
    sze_md::SzeHpfSnapshot wire{};
    wire.head = make_head(sze_md::kSnapshotMessage, 3001);
    wire.last_price = 123450;
    wire.pre_close_price = 120000;
    wire.open_price = 121000;
    wire.day_high_price = 124000;
    wire.day_low_price = 119000;
    wire.total_quantity = 123400;
    wire.total_value = 1234567800ULL;
    wire.upper_limit_price = 132000;
    wire.low_limit_price = 108000;
    wire.bid[0].price = 123440;
    wire.bid[0].quantity = 12300;
    wire.ask[0].price = 123460;
    wire.ask[0].quantity = 45600;

    LFMarketDataField result{};
    assert(sze_md::decode_snapshot(&wire, sizeof(wire), &result));
    assert(std::strcmp(result.InstrumentID, "000001") == 0);
    assert(std::strcmp(result.ExchangeID, "SZE") == 0);
    assert(std::strcmp(result.TradingDay, "20260720") == 0);
    assert(std::strcmp(result.UpdateTime, "14:30:15.123") == 0);
    assert(std::fabs(result.LastPrice - 12.345) < 1e-12);
    assert(std::fabs(result.BidPrice1 - 12.344) < 1e-12);
    assert(result.BidVolume1 == 123);
    assert(std::fabs(result.AskPrice1 - 12.346) < 1e-12);
    assert(result.AskVolume1 == 456);
    assert(result.aBidVolume[0] == result.BidVolume1);
    assert(result.Volume == 1234);
    assert(std::fabs(result.Turnover - 1234.5678) < 1e-12);
    assert(!sze_md::decode_snapshot(&wire, sizeof(wire) - 1, &result));
}

void test_heartbeat_and_rejection()
{
    const std::uint8_t heartbeat_types[] = {
        sze_md::kSnapshotHeartbeatMessage,
        sze_md::kIndexHeartbeatMessage,
        sze_md::kTickHeartbeatMessage,
        sze_md::kAfterCloseHeartbeatMessage,
        sze_md::kTurnoverHeartbeatMessage,
        sze_md::kIbrTreeHeartbeatMessage,
        sze_md::kTreeHeartbeatMessage,
        sze_md::kBondSnapshotHeartbeatMessage,
        sze_md::kBondTickHeartbeatMessage,
    };
    for (std::size_t index = 0;
         index < sizeof(heartbeat_types) / sizeof(heartbeat_types[0]); ++index) {
        sze_md::SzeHpfHeartbeat heartbeat{};
        heartbeat.message_type = heartbeat_types[index];
        assert(sze_md::wire_record_size(heartbeat.message_type) ==
               sizeof(heartbeat));
        assert(sze_md::decode_record(&heartbeat, sizeof(heartbeat), 0, 0) ==
               sze_md::DecodeStatus::kHeartbeat);
    }

    sze_md::SzeHpfOrder bond_order{};
    bond_order.head.message_type = sze_md::kBondOrderMessage;
    assert(sze_md::decode_record(&bond_order, sizeof(bond_order), 0, 0) ==
           sze_md::DecodeStatus::kKnownNonTarget);
    sze_md::SzeHpfExecution bond_execution{};
    bond_execution.head.message_type = sze_md::kBondExecutionMessage;
    assert(sze_md::decode_record(&bond_execution, sizeof(bond_execution), 0, 0) ==
           sze_md::DecodeStatus::kKnownNonTarget);

    assert(sze_md::wire_record_size(sze_md::kSnapshotMessage) == 376U);
    assert(sze_md::wire_record_size(sze_md::kIndexMessage) == 96U);
    assert(sze_md::wire_record_size(sze_md::kAfterCloseMessage) == 96U);
    assert(sze_md::wire_record_size(sze_md::kTurnoverMessage) == 80U);
    assert(sze_md::wire_record_size(sze_md::kIbrTreeMessage) == 200U);
    assert(sze_md::wire_record_size(sze_md::kTreeMessage) == 392U);
    assert(sze_md::wire_record_size(sze_md::kBondSnapshotMessage) == 400U);
    assert(sze_md::wire_record_size(99U) == 0U);
    assert(static_cast<int>(sze_md::DecodeStatus::kOrder) == 0);
    assert(static_cast<int>(sze_md::DecodeStatus::kExecution) == 1);
    assert(static_cast<int>(sze_md::DecodeStatus::kHeartbeat) == 2);
    assert(static_cast<int>(sze_md::DecodeStatus::kUnknown) == 3);
    assert(static_cast<int>(sze_md::DecodeStatus::kMalformed) == 4);

    sze_md::SzeHpfHeartbeat unknown_control{};
    unknown_control.message_type = 199U;
    sze_md::DecodeFailureReason control_failure =
        sze_md::DecodeFailureReason::kNone;
    assert(sze_md::decode_record(&unknown_control, sizeof(unknown_control), 0, 0,
                                 &control_failure) ==
           sze_md::DecodeStatus::kUnknown);
    assert(control_failure ==
           sze_md::DecodeFailureReason::kUnexpectedHeartbeatType);

    sze_md::SzeHpfOrder malformed{};
    malformed.head = make_head(sze_md::kOrderMessage, 3001);
    malformed.order_quantity = 1;
    malformed.side_flag = 'X';
    malformed.order_type = '2';
    LFL2OrderField result{};
    sze_md::DecodeFailureReason failure = sze_md::DecodeFailureReason::kNone;
    assert(sze_md::decode_record(&malformed, sizeof(malformed), &result, 0) ==
           sze_md::DecodeStatus::kMalformed);
    assert(sze_md::decode_record(&malformed, sizeof(malformed), &result, 0,
                                 &failure) == sze_md::DecodeStatus::kMalformed);
    assert(failure == sze_md::DecodeFailureReason::kInvalidOrderSide);
    assert(sze_md::decode_record(&malformed, sizeof(malformed) - 1, &result, 0) ==
           sze_md::DecodeStatus::kMalformed);
    malformed.head.message_type = 99;
    assert(sze_md::decode_record(&malformed, sizeof(malformed), &result, 0,
                                 &failure) == sze_md::DecodeStatus::kUnknown);
    assert(failure == sze_md::DecodeFailureReason::kUnsupportedMessageType);
    assert(std::strcmp(sze_md::decode_failure_reason_name(failure),
                       "unsupported-message-type") == 0);

    malformed.head.message_type = sze_md::kTickHeartbeatMessage;
    assert(sze_md::decode_record(&malformed, sizeof(malformed), &result, 0,
                                 &failure) == sze_md::DecodeStatus::kMalformed);
    assert(failure == sze_md::DecodeFailureReason::kInvalidLength);

    // The EFH contract permits the leap-second representation SS=60.
    malformed.head = make_head(sze_md::kOrderMessage, 3002);
    malformed.head.quote_update_time = 20260720145960000ULL;
    malformed.side_flag = '1';
    malformed.order_type = '2';
    malformed.order_quantity = 1;
    assert(sze_md::decode_record(&malformed, sizeof(malformed), &result, 0) ==
           sze_md::DecodeStatus::kOrder);

    // A failed decode must not partially overwrite its caller-owned output.
    LFL2OrderField unchanged{};
    std::strncpy(unchanged.InstrumentID, "keep", sizeof(unchanged.InstrumentID) - 1);
    malformed.side_flag = 'X';
    assert(sze_md::decode_record(&malformed, sizeof(malformed), &unchanged, 0) ==
           sze_md::DecodeStatus::kMalformed);
    assert(std::strcmp(unchanged.InstrumentID, "keep") == 0);
}

void test_recovery_uses_live_decoder()
{
    sze_md::SzeHpfOrder wire{};
    wire.head = make_head(sze_md::kOrderMessage, 4001);
    wire.order_price = 123450;
    wire.order_quantity = 2500;
    wire.side_flag = '1';
    wire.order_type = '2';

    sze_recovery::CanonicalEvent event{};
    event.event_id = 1U;
    event.feed_sequence = wire.head.sequence;
    event.channel_sequence = wire.head.sequence_num;
    event.receive_mono_ns = 123U;
    event.exchange_time = wire.head.quote_update_time;
    event.trading_day = 20260720U;
    event.payload_crc32 = sze_recovery::crc32(&wire, sizeof(wire));
    event.source_id = 88U;
    event.channel_number = wire.head.channel_num;
    event.payload_size = sizeof(wire);
    event.message_type = wire.head.message_type;
    event.record_kind = sze_recovery::kRecordMarketData;

    LFL2OrderField live{};
    LFL2OrderField replay{};
    assert(sze_md::decode_record(&wire, sizeof(wire), &live, 0) ==
           sze_md::DecodeStatus::kOrder);
    assert(sze_md::decode_recovery_record(event, &wire, sizeof(wire), &replay, 0) ==
           sze_md::DecodeStatus::kOrder);
    assert(std::memcmp(&live, &replay, sizeof(live)) == 0);

    event.feed_sequence += 1U;
    LFL2OrderField unchanged{};
    std::strncpy(unchanged.InstrumentID, "keep",
                 sizeof(unchanged.InstrumentID) - 1U);
    assert(sze_md::decode_recovery_record(
               event, &wire, sizeof(wire), &unchanged, 0) ==
           sze_md::DecodeStatus::kMalformed);
    assert(std::strcmp(unchanged.InstrumentID, "keep") == 0);
}

}  // namespace

int main()
{
    test_order();
    test_fill_and_cancel();
    test_snapshot();
    test_heartbeat_and_rejection();
    test_recovery_uses_live_decoder();
    return 0;
}
