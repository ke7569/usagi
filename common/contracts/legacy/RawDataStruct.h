//
// Created by Administrator on 2024/1/4.
//

#ifndef MS_MSDATASTRUCT_H
#define MS_MSDATASTRUCT_H
#define TRADE_TIME_LENGTH 12
#define EXCHANGE_ID_LENGTH 4
#define INSTRUMENT_ID_LENGTH 6
#define ORDER_KIND_LENGTH 1
#define ORDER_TYPE_LENGTH 1
#define ORDER_BS_FLAG 1
#define BEST_PRICE (-1.0)
#define MARKET_PRICE (99999000.0)
#define PRICE_LEVEL 10
#define PRICE_MULTIPLIER 1000.0

// #include <sstream>
// #include <vector>
#include <array>
// #include <thread>
// #include <mutex>
// #include <unordered_map>
// #include <cstring>
#include "common/contracts/legacy/LFDataStruct.h"
#include <ostream>
#include <cfloat>
enum MarketDataIndex{
    InstrumentIDIndex,
    MarketTimeIndex,
    LastPriceIndex,
    MidPriceIndex,
    VolumeIndex,
    TurnoverIndex,
    BidVolume1Index,
    BidVolume2Index,
    BidVolume3Index,
    BidVolume4Index,
    BidVolume5Index,
    BidVolume6Index,
    BidVolume7Index,
    BidVolume8Index,
    BidVolume9Index,
    BidVolumeAIndex,
    AskVolume1Index,
    AskVolume2Index,
    AskVolume3Index,
    AskVolume4Index,
    AskVolume5Index,
    AskVolume6Index,
    AskVolume7Index,
    AskVolume8Index,
    AskVolume9Index,
    AskVolumeAIndex,
    BidPrice1Index,
    BidPrice2Index,
    BidPrice3Index,
    BidPrice4Index,
    BidPrice5Index,
    BidPrice6Index,
    BidPrice7Index,
    BidPrice8Index,
    BidPrice9Index,
    BidPriceAIndex,
    AskPrice1Index,
    AskPrice2Index,
    AskPrice3Index,
    AskPrice4Index,
    AskPrice5Index,
    AskPrice6Index,
    AskPrice7Index,
    AskPrice8Index,
    AskPrice9Index,
    AskPriceAIndex,
    OrderPriceIndex,
    OrderVolumeIndex,
    CancelPriceIndex,
    CancelVolumeIndex,
    IsSellIndex,
    IsCancelIndex,
    AppSeqIndex,
    BASIC_FIELD_NUM
};

// BSE 5档盘口数据结构
enum BseSnapshotIndex {
    BseInstrumentIDIndex,
    BseMarketTimeIndex,
    BseLastPriceIndex,
    BseMidPriceIndex,
    BseVolumeIndex,
    BseTurnoverIndex,
    BseBidVolume1Index,
    BseBidVolume2Index,
    BseBidVolume3Index,
    BseBidVolume4Index,
    BseBidVolume5Index,
    BseAskVolume1Index,
    BseAskVolume2Index,
    BseAskVolume3Index,
    BseAskVolume4Index,
    BseAskVolume5Index,
    BseBidPrice1Index,
    BseBidPrice2Index,
    BseBidPrice3Index,
    BseBidPrice4Index,
    BseBidPrice5Index,
    BseAskPrice1Index,
    BseAskPrice2Index,
    BseAskPrice3Index,
    BseAskPrice4Index,
    BseAskPrice5Index,
    BSE_SNAPSHOT_FIELD_NUM
};


struct MSMarketData;
struct BseSnapshot;
union MSMarketDataField;
union BseSnapshotField;
struct OrderDataCollection{
    const uint64_t *OrderTime;
    const int64_t*ExchangeID;
    const int64_t*InstrumentID;
    const double *Price;
    const double *Volume;
    const int64_t * OrderKind;
    const int64_t *ApplSeqNum;
    const int64_t * OrdType;
    const int64_t *OrderNo;
    const int64_t* BizIndex;
    const int64_t* IsLast;
    const int64_t* mRank;
    const int64_t OrderLength;

    OrderDataCollection(const uint64_t * order_time,
                        const int64_t * exchange_id,
                        const int64_t * instrument_id,
                        const double * price,
                        const double * volume,
                        const int64_t * order_kind,
                        const int64_t* appl_seq_num,
                        const int64_t * ord_type,
                        const int64_t*order_no,
                        const int64_t*biz_index,
                        const int64_t* is_last,
                        const int64_t *rank,
                        int64_t order_length);

    LFL2OrderField operator[](long index) const;
    size_t size() const;
};
struct TradeDataCollection{

public:
    const uint64_t *TradeTime;
    const int64_t *ExchangeID;
    const int64_t *InstrumentID;
    const double *Price;
    const double *Volume;
    const int64_t *OrderKind;
    const int64_t *OrderBSFlag;
    const double *TurnOver;
    const int64_t *BidApplSeqNum;
    const int64_t *OfferApplSeqNum;
    const int64_t *ApplSeqNum;
    const int64_t *BizIndex;
    const int64_t *IsLast;
    const int64_t *mRank;
    const int64_t TradeLength;
    TradeDataCollection(const uint64_t *tradeTime,
                        const int64_t *exchangeID,
                        const int64_t *instrumentID,
                        const double *price,
                        const double *volume,
                        const int64_t *orderKind,
                        const int64_t *orderBSFlag,
                        const double *turnOver,
                        const int64_t *bidApplSeqNum,
                        const int64_t *offerApplSeqNum,
                        const int64_t *applSeqNum,
                        const int64_t *bizIndex,
                        const int64_t *isLast,
                        const int64_t *rank,
                        int64_t tradeLength);

    LFL2TradeField operator[](long index) const;
    size_t size() const;

};


struct BseSnapshotCollection {
public:
    const uint64_t *UpdateTime;
    const char* const* InstrumentID;
    const char* const* ExchangeID;
    const double *LastPrice;
    const double *PreClosePrice;
    const double *OpenPrice;
    const double *HighestPrice;
    const double *LowestPrice;
    const int *Volume;
    const double *Turnover;
    const double *BidPrice1;
    const int *BidVolume1;
    const double *AskPrice1;
    const int *AskVolume1;
    const double *BidPrice2;
    const int *BidVolume2;
    const double *AskPrice2;
    const int *AskVolume2;
    const double *BidPrice3;
    const int *BidVolume3;
    const double *AskPrice3;
    const int *AskVolume3;
    const double *BidPrice4;
    const int *BidVolume4;
    const double *AskPrice4;
    const int *AskVolume4;
    const double *BidPrice5;
    const int *BidVolume5;
    const double *AskPrice5;
    const int *AskVolume5;
    const int64_t *mRank;
    const int64_t SnapshotLength;

    BseSnapshotCollection(const uint64_t *updateTime,
                        const char* const* instrumentID,
                        const char* const* exchangeID,
                        const double *lastPrice,
                        const double *preClosePrice,
                        const double *openPrice,
                        const double *highestPrice,
                        const double *lowestPrice,
                        const int *volume,
                        const double *turnover,
                        const double *bidPrice1,
                        const int *bidVolume1,
                        const double *askPrice1,
                        const int *askVolume1,
                        const double *bidPrice2,
                        const int *bidVolume2,
                        const double *askPrice2,
                        const int *askVolume2,
                        const double *bidPrice3,
                        const int *bidVolume3,
                        const double *askPrice3,
                        const int *askVolume3,
                        const double *bidPrice4,
                        const int *bidVolume4,
                        const double *askPrice4,
                        const int *askVolume4,
                        const double *bidPrice5,
                        const int *bidVolume5,
                        const double *askPrice5,
                        const int *askVolume5,
                        const int64_t *rank,
                        int64_t snapshotLength);

    BseSnapshotField operator[](long index) const;
    size_t size() const;
};







struct  MSQuoteData{
    bool is_sell;
    bool is_cancel;
    char InstrumentID[31];
    char QuoteTime[31];
    double Price;
    double Volume;
    long ApplSeqNum;
    long QuoteTag;
    long OrigTime;
    // 声明友元函数，以便能够访问类的私有成员
    friend std::ostream& operator<<(std::ostream& os, const MSQuoteData& data);

};
struct MSMarketData{
    std::array<double,BASIC_FIELD_NUM> ms_market_data{};

    // 构造函数
    MSMarketData()
    {
        ms_market_data[InstrumentIDIndex]=0.0;
        ms_market_data[MarketTimeIndex]=0.0;
        ms_market_data[LastPriceIndex]=DBL_MAX;
        ms_market_data[VolumeIndex]=0.0;
        ms_market_data[TurnoverIndex]=0.0;
        ms_market_data[MidPriceIndex]=DBL_MAX;
        for(int i=BidVolume1Index;i<=AskVolumeAIndex;i++){
            ms_market_data[i]=0.0;
        }
        for(int i=BidPrice1Index;i<=AskPriceAIndex;i++){
            ms_market_data[i]=DBL_MAX;
        }
        ms_market_data[CancelPriceIndex]=DBL_MAX;
        ms_market_data[OrderPriceIndex]=DBL_MAX;
        ms_market_data[CancelVolumeIndex]=0.0;
        ms_market_data[OrderVolumeIndex]=0.0;
        ms_market_data[IsSellIndex]=0.0;
        ms_market_data[IsCancelIndex]=0.0;
        ms_market_data[AppSeqIndex]=0.0;
    }
};

struct BseSnapshot {
    std::array<double, BSE_SNAPSHOT_FIELD_NUM> bse_snapshot_data{};

    // 构造函数
    BseSnapshot()
    {
        bse_snapshot_data[BseInstrumentIDIndex] = 0.0;
        bse_snapshot_data[BseMarketTimeIndex] = 0.0;
        bse_snapshot_data[BseLastPriceIndex] = DBL_MAX;
        bse_snapshot_data[BseVolumeIndex] = 0.0;
        bse_snapshot_data[BseTurnoverIndex] = 0.0;
        bse_snapshot_data[BseMidPriceIndex] = DBL_MAX;
        for(int i = BseBidVolume1Index; i <= BseAskVolume5Index; i++){
            bse_snapshot_data[i] = 0.0;
        }
        for(int i = BseBidPrice1Index; i <= BseAskPrice5Index; i++){
            bse_snapshot_data[i] = DBL_MAX;
        }
    }
};

union MSMarketDataField{
    MSMarketData ms_market_data;
    struct {
        double InstrumentID;
        double MarketTime;
        double LastPrice;
        double MidPrice;
        double Volume;
        double Turnover;
        double BidVolume1;
        double BidVolume2;
        double BidVolume3;
        double BidVolume4;
        double BidVolume5;
        double BidVolume6;
        double BidVolume7;
        double BidVolume8;
        double BidVolume9;
        double BidVolumeA;
        double AskVolume1;
        double AskVolume2;
        double AskVolume3;
        double AskVolume4;
        double AskVolume5;
        double AskVolume6;
        double AskVolume7;
        double AskVolume8;
        double AskVolume9;
        double AskVolumeA;
        double BidPrice1;
        double BidPrice2;
        double BidPrice3;
        double BidPrice4;
        double BidPrice5;
        double BidPrice6;
        double BidPrice7;
        double BidPrice8;
        double BidPrice9;
        double BidPriceA;
        double AskPrice1;
        double AskPrice2;
        double AskPrice3;
        double AskPrice4;
        double AskPrice5;
        double AskPrice6;
        double AskPrice7;
        double AskPrice8;
        double AskPrice9;
        double AskPriceA;
        double OrderPrice;
        double OrderVolume;
        double CancelPrice;
        double CancelVolume;
        double IsSell;
        double IsCancel;
        double AppSeq;
    };
    std::string to_string() const;
};

union BseSnapshotField {
    BseSnapshot bse_snapshot;
    struct {
        int32_t InstrumentID;
        double MarketTime;
        double LastPrice;
        double MidPrice;
        double Volume;
        double Turnover;
        double BidVolume1;
        double BidVolume2;
        double BidVolume3;
        double BidVolume4;
        double BidVolume5;
        double AskVolume1;
        double AskVolume2;
        double AskVolume3;
        double AskVolume4;
        double AskVolume5;
        double BidPrice1;
        double BidPrice2;
        double BidPrice3;
        double BidPrice4;
        double BidPrice5;
        double AskPrice1;
        double AskPrice2;
        double AskPrice3;
        double AskPrice4;
        double AskPrice5;
    };
    std::string to_string() const;
};







#endif //MS_MSDATASTRUCT_H
