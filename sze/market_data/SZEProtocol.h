#ifndef DEEPWIN_SZE_PROTOCOL_H
#define DEEPWIN_SZE_PROTOCOL_H

#include "longfist/LFDataStruct.h"
#include "sze/market_data/SZERecoverable.h"

#include <cstddef>
#include <cstdint>

namespace sze_md {

// Shengli EFH SZE wire records. The feed is little-endian on the supported
// x86 hosts. Datagram records are framed by message type, not one fixed size.
enum : std::uint8_t {
    kSnapshotMessage = 21,
    kIndexMessage = 22,
    kOrderMessage = 23,
    kExecutionMessage = 24,
    kAfterCloseMessage = 25,
    kTurnoverMessage = 26,
    kIbrTreeMessage = 28,
    kTreeMessage = 29,
    kBondSnapshotMessage = 80,
    kBondOrderMessage = 81,
    kBondExecutionMessage = 82,
    kSnapshotHeartbeatMessage = 121,
    kIndexHeartbeatMessage = 122,
    kTickHeartbeatMessage = 123,
    kAfterCloseHeartbeatMessage = 125,
    kTurnoverHeartbeatMessage = 126,
    kIbrTreeHeartbeatMessage = 128,
    kTreeHeartbeatMessage = 129,
    kBondSnapshotHeartbeatMessage = 180,
    kBondTickHeartbeatMessage = 181,
};

enum : std::size_t {
    kHeartbeatRecordSize = 16,
    kHeadRecordSize = 43,
    kSnapshotRecordSize = 376,
    kIndexRecordSize = 96,
    kOrderRecordSize = 72,
    kExecutionRecordSize = 72,
    kAfterCloseRecordSize = 96,
    kTurnoverRecordSize = 80,
    kIbrTreeRecordSize = 200,
    kTreeRecordSize = 392,
    kBondSnapshotRecordSize = 400,
    kBondOrderRecordSize = 72,
    kBondExecutionRecordSize = 72,
};

#pragma pack(push, 1)
struct SzeHpfHead {
    std::uint32_t sequence;
    std::uint32_t reserved_1;
    std::uint8_t message_type;
    std::uint8_t security_type;
    std::uint8_t sub_security_type;
    std::uint8_t symbol[9];
    std::uint8_t exchange_id;
    std::uint64_t quote_update_time;
    std::uint16_t channel_num;
    std::uint64_t sequence_num;
    std::uint32_t md_stream_id;
};

struct SzeHpfOrder {
    SzeHpfHead head;
    std::uint32_t order_price;
    std::uint64_t order_quantity;
    char side_flag;
    char order_type;
    char reserved[15];
};

struct SzeHpfExecution {
    SzeHpfHead head;
    std::int64_t trade_buy_num;
    std::int64_t trade_sell_num;
    std::uint32_t trade_price;
    std::int64_t trade_quantity;
    char trade_type;
};

struct SzeHpfSnapshot {
    SzeHpfHead head;
    std::uint8_t trading_status;
    std::uint64_t total_trade_num;
    std::uint64_t total_quantity;
    std::uint64_t total_value;
    std::uint32_t pre_close_price;
    std::uint32_t last_price;
    std::uint32_t open_price;
    std::uint32_t day_high_price;
    std::uint32_t day_low_price;
    std::uint32_t today_close_price;
    std::uint32_t total_bid_weighted_avg_price;
    std::uint64_t total_bid_quantity;
    std::uint32_t total_ask_weighted_avg_price;
    std::uint64_t total_ask_quantity;
    std::uint32_t lpv;
    std::uint32_t iopv;
    std::uint32_t upper_limit_price;
    std::uint32_t low_limit_price;
    std::uint32_t open_interest;
    struct PriceQuantity {
        std::uint32_t price;
        std::uint64_t quantity;
    } bid[10], ask[10];
};

struct SzeHpfHeartbeat {
    std::uint32_t sequence;
    std::uint32_t reserved_1;
    std::uint8_t message_type;
    std::uint8_t reserved_2[7];
};
#pragma pack(pop)

static_assert(sizeof(SzeHpfHead) == 43, "unexpected SZE head size");
static_assert(sizeof(SzeHpfOrder) == 72, "unexpected SZE order size");
static_assert(sizeof(SzeHpfExecution) == 72, "unexpected SZE execution size");
static_assert(sizeof(SzeHpfSnapshot) == kSnapshotRecordSize,
              "unexpected SZE snapshot size");
static_assert(sizeof(SzeHpfHeartbeat) == 16, "unexpected SZE heartbeat size");

enum class DecodeStatus {
    kOrder = 0,
    kExecution = 1,
    kHeartbeat = 2,
    kUnknown = 3,
    kMalformed = 4,
    kKnownNonTarget = 5,
};

// Coarse DecodeStatus is retained for callback compatibility. This reason is
// populated on every non-success path so capture diagnostics can distinguish a
// bad length from a valid-length record with an unsupported type or field.
enum class DecodeFailureReason : std::uint8_t {
    kNone = 0,
    kNullRecord,
    kInvalidLength,
    kUnexpectedHeartbeatType,
    kUnsupportedMessageType,
    kMissingOrderOutput,
    kInvalidOrderExchange,
    kInvalidOrderSequence,
    kInvalidOrderSide,
    kInvalidOrderType,
    kInvalidOrderQuantity,
    kInvalidOrderSymbol,
    kInvalidOrderTime,
    kMissingTradeOutput,
    kInvalidTradeExchange,
    kInvalidTradeSequence,
    kInvalidTradeType,
    kInvalidTradeIds,
    kInvalidTradeQuantity,
    kInvalidTradePrice,
    kInvalidTradeSymbol,
    kInvalidTradeTime,
    kInvalidRecoveryEnvelope,
};

// Decode one complete, already framed wire record. Equity output structures
// are modified only for successful type-23/type-24 normalization.
DecodeStatus decode_record(const void* record,
                           std::size_t length,
                           LFL2OrderField* order,
                           LFL2TradeField* trade,
                           DecodeFailureReason* failure = 0);

bool decode_snapshot(const void* record, std::size_t length,
                     LFMarketDataField* snapshot);

// Replay validates the persisted envelope, then enters the exact decoder used
// by live packets so normalization cannot drift between the two paths.
DecodeStatus decode_recovery_record(const sze_recovery::CanonicalEvent& event,
                                    const void* record,
                                    std::size_t length,
                                    LFL2OrderField* order,
                                    LFL2TradeField* trade,
                                    DecodeFailureReason* failure = 0);

// Returns zero for message types not present in the official protocol table.
std::size_t wire_record_size(std::uint8_t message_type);
bool is_official_heartbeat(std::uint8_t message_type);

const char* decode_status_name(DecodeStatus status);
const char* decode_failure_reason_name(DecodeFailureReason reason);

enum : std::uint64_t {
    kDiagnosticFileMagic = 0x3147414944455a53ULL,  // "SZEDIAG1"
    kDiagnosticRecordCommit = 0x314d4f4347414944ULL,
};
enum : std::uint32_t {
    kDiagnosticFileVersion = 1,
    kDiagnosticPayloadBytes = 128,
};
enum : std::uint8_t {
    kDiagnosticPayloadTruncated = 1U << 0U,
    kDiagnosticNonInvalidating = 1U << 1U,
};

struct DiagnosticFileHeader {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t record_size;
    std::uint32_t capacity;
    std::uint64_t committed_records;
    std::uint64_t dropped_records;
    std::uint64_t created_realtime_ns;
    std::uint32_t trading_day;
    std::uint16_t source_id;
    std::uint16_t reserved_1;
    char build_id[64];
    std::uint8_t reserved_2[136];
};

struct DiagnosticRecord {
    std::uint64_t diagnostic_realtime_ns;
    std::uint64_t receive_mono_ns;
    std::uint64_t packet_number;
    std::uint64_t channel_sequence;
    std::uint64_t exchange_time;
    std::uint64_t feed_sequence;
    std::uint32_t channel_index;
    std::uint32_t record_offset;
    std::uint32_t datagram_length;
    std::uint32_t record_length;
    std::uint16_t channel_number;
    std::uint16_t captured_length;
    std::uint8_t decode_status;
    std::uint8_t failure_reason;
    std::uint8_t message_type;
    std::uint8_t flags;
    std::uint8_t payload[kDiagnosticPayloadBytes];
    std::uint8_t reserved[48];
    std::uint64_t commit;
};

static_assert(sizeof(DiagnosticFileHeader) == 256,
              "unexpected SZE diagnostic header size");
static_assert(sizeof(DiagnosticRecord) == 256,
              "unexpected SZE diagnostic record size");
static_assert(offsetof(DiagnosticRecord, commit) == 248,
              "SZE diagnostic commit must remain naturally aligned");

}  // namespace sze_md

#endif  // DEEPWIN_SZE_PROTOCOL_H
