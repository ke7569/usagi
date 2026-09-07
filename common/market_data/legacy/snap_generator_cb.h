//
// Created by Administrator on 2024/1/15.
//

#ifndef RAW_DATA_GENERATOR_SNAP_GENERATOR_H
#define RAW_DATA_GENERATOR_SNAP_GENERATOR_H
#include "common/contracts/legacy/RawDataStruct.h"
#include "common/market_data/legacy/order_match_engine.h"
#include <unordered_set>
#include <functional>
#include <map>
#include <iostream>
#include <cmath>
#define MARKET_ARRAY_LENGTH 3
#define PRICE_MULTIPLIER 1000.0
enum class MarketType{
    SH_STOCK_MARKET,
    SZ_STOCK_MARKET,
    SH_CB_MARKET,
    SZ_CB_MARKET,
    NONE
};

// 自定义哈希函数
struct HashFunc {
    std::size_t operator()(const char* s) const {
        std::size_t hash = 0;
        for(int i=0;i<2;i++) {
            hash = hash * 101 + s[i]; // 101是一个质数，可以是其他值
        }
        return hash;
    }
};

// 自定义相等函数
struct EqualFunc {
    bool operator()(const char* s1, const char* s2) const {
        if((s1[0]==s2[0]) &&(s1[1]==s2[1])){
            return true;
        }else{
            return false;
        }
    }
};
// 自定义哈希函数
struct HashCodeFunc {
    std::size_t operator()(const char* s) const {
        std::size_t hash = 0;
        for(int i=0;i<6;i++) {
            hash = hash * 10000003 + s[i]; // 101是一个质数，可以是其他值
        }
        return hash;
    }
};

// 自定义相等函数
struct EqualCodeFunc {
    bool operator()(const char* s1, const char* s2) const {
        if(strcmp(s1,s2)==0){
            return true;
        }else{
            return false;
        }
    }
};

class SnapGenerator{
public:
//    double* mMarketDataPtr;
    bool mIsMarketPriceOrder{};
//    MSMarketData mMsMarketData{};
    std::array<MSMarketDataField,MARKET_ARRAY_LENGTH> mMsMarketDataFieldArray{};
    bool mIsBestPriceOrder{};
    MSQuoteData mCurrentQuoteData{};
    std::vector<std::string> mStockCodeVec;
    std::unordered_map<const char*,OrderMatchEngine,HashCodeFunc,EqualCodeFunc> mOrderMatchEngineMap;
    long mSnapIndex;
    char mCurrentInstrumentID[31]{};
    std::map<int,int> mAskBook;
    std::map<int,int> mBidBook;

    std::unordered_set<std::string> mPrintInstrumentIDSet;

    SnapGenerator();



    explicit SnapGenerator(const std::string& instrument_id);


    MarketType judge_market_type(const char*instrument_id);
    bool is_best_price_empty(MSQuoteData&quote_data);
    static bool is_market_price_cancel(MSQuoteData&quote_data,const LFL2TradeField*trade_data);
    void insert_quote_data(MSQuoteData&quote_data);
    size_t get_snap_data_length() const;
//    void assign_ms_market_data(long index,const MSMarketData&market_data) const;
    void process_trade(const LFL2TradeField*trade_data);
    void process_order(const LFL2OrderField*order_data);
    void process_sh_stock_trade(const LFL2TradeField*trade_data);
    void process_sz_stock_trade(const LFL2TradeField*trade_data);
    void process_sh_stock_auction_trade(const LFL2TradeField*trade_data);
    void process_sz_stock_auction_trade(const LFL2TradeField*trade_data);
    void process_sh_stock_non_auction_trade(const LFL2TradeField*trade_data);
    void process_sz_stock_non_auction_trade(const LFL2TradeField*trade_data);
    void process_sh_stock_order(const LFL2OrderField*order_data);
    void process_sz_stock_order(const LFL2OrderField*order_data);
    void process_sz_stock_market_order(const LFL2OrderField*order_data);
    static bool is_auction_time(const char*time);
//    bool is_done() const;
    void assign_snap_market_data();
//    void next();
    std::unordered_set<const char*,HashFunc,EqualFunc> mSHStockInstrumentUnorderedSet={"60","68"};
    std::unordered_set<const char*,HashFunc,EqualFunc> mSZStockInstrumentUnorderedSet={"00","30"};
    std::unordered_set<const char*,HashFunc,EqualFunc> mSZCBInstrumentUnorderedSet={"12"};
    std::unordered_set<const char*,HashFunc,EqualFunc> mSHCBInstrumentUnorderedSet={"11"};

};
#endif //RAW_DATA_GENERATOR_SNAP_GENERATOR_H
