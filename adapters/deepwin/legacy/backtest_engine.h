//
// Created by admin on 2023/11/3.
//

#ifndef TEST_ORDER_BACKTEST_ENGINE_H
#define TEST_ORDER_BACKTEST_ENGINE_H

// #include <chrono>
#include <iomanip>
#include <set>
#include <unordered_set>
#include <cmath>
#include <algorithm>

#include "adapters/deepwin/legacy/event_handler.h"
//#include "strategy.h"
#include "common/market_data/legacy/snap_generator.h"
#include "adapters/deepwin/legacy/wc_strategy.h"
#include "common/market_data/legacy/order_match_engine.h"
// #include "helper.h"
// #include <chrono>
// #include <sstream>
#include <unordered_map>
// #include <condition_variable>
//#include <longfist/LFDataStruct.h>
#include "common/contracts/legacy/LFDataStruct.h"
#include "third_party/nlohmann/json.hpp"
#include "common/contracts/legacy/RawDataStruct.h"
#include <functional>

// 自定义哈希函数
//struct HashFunc {
//    std::size_t operator()(const char* s) const {
//        std::size_t hash = 0;
//        for(int i=0;i<2;i++) {
//            hash = hash * 101 + s[i]; // 101是一个质数，可以是其他值
//        }
//        return hash;
//    }
//};
//
//// 自定义相等函数
//struct EqualFunc {
//    bool operator()(const char* s1, const char* s2) const {
//        if((s1[0]==s2[0]) &&(s1[1]==s2[1])){
//            return true;
//        }else{
//            return false;
//        }
//    }
//};
using nlohmann::json;
struct  CancelOrderStruct{
    short source;
    int request_id;

};
struct InsertLimitOrderStruct{
    short source;
    std::string ticker;
    std::string exchange_id;
    double price;
    int volume;
    LfDirectionType direction;
    LfOffsetFlagType offset;
    int request_id;
};
struct TickInfoStruct{
    std::string  InstrumentID;
    int tick_index;
};
//#include "cling_init_function.h"
typedef void (*BackTestDW_C_on_market_data_level2)(const struct LFL2MarketDataField* data, short source, long rcv_time);
typedef void (*BackTestDW_C_on_market_data)(const struct LFMarketDataField* data, short source, long rcv_time);
typedef void (*BackTestDW_C_on_rtn_trade)(const struct LFRtnTradeField* data, int request_id, short source, long rcv_time);
typedef void (*BackTestDW_C_on_rtn_order)(const struct LFRtnOrderField* data, int request_id, short source, long rcv_time);
typedef void (*BackTestDW_C_on_l2_trade)(const struct LFL2TradeField* data, short source, long rcv_time);
typedef void (*BackTestDW_C_on_l2_order)(const struct LFL2OrderField* data, short source, long rcv_time);
typedef void (*BackTestDW_C_on_check_pending_order)();

//long long getNanoTime() {
//    auto now = std::chrono::high_resolution_clock::now();
//    auto now_ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(now);
//    auto epoch = now_ns.time_since_epoch();
//    auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch);
//    return value.count();
//}
class IWCStrategy;
class EventEngine;
class BacktestEngine {
public:
    std::unordered_map<std::string,long> _sys_rid_map;
//    std::mutex mMtx;

    IWCStrategy*mStrategy;
    LFMarketDataField* mTickData{};
    std::unordered_map<std::string,LFMarketDataField*> mCurrentTickDataMap;
    std::unordered_map<std::string,LFMarketDataField*> mNextTickDataMap;
    int mLength{};
    std::unordered_map<int,LFRtnOrderField> mRequestIdOrderMap{};
    std::unordered_map<std::string,std::unordered_map<int,MSQuoteData>> mRankQuoteDataMap;
    std::unordered_map<std::string,std::unordered_map<long long,int>> mNanoTickDataIndexMap;
    std::unordered_map<std::string,std::unordered_map<long long,int>> mRankOrderDataIndexMap;
    std::unordered_map<std::string,std::unordered_map<long long,int>> mRankQuoteDataIndexMap;
    std::unordered_map<std::string,std::unordered_map<long long,int>> mRankTradeDataIndexMap;
    std::unordered_map<std::string,std::unordered_map<long long,int>> mRankMarketDataIndexMap;
    std::unordered_map<std::string,std::unordered_map<long long,bool>> mRankIsStrategyMap;
    std::unordered_map<std::string,OrderMatchEngine> mOrderMatchEngineMap;
    std::unordered_map<std::string,OrderMatchEngine> mModifiedOrderMatchEngineMap;

    std::unordered_map<int,std::function<void()>> mRankCallBackFunc;//定时器与instrument_id无关，跟rank有关，直接用mRank，全局唯一变量
    EventEngine * mEventEngine;
    long long mCurrentNanoTime{};//去掉SameNanoTimeCount，用另外的结构体保证
    long long mOrderTradeDataNanoTime{};//去掉SameNanoTimeCount，用另外的结构体保证
    long long mMarketDataNanoTime{};
//    long long mSameNanoTimeCount;
//    std::vector<std::string> mInstrumentIdVec;
    std::unordered_set<std::string> mUniqueInstrumentIdSet;
    std::vector<long long> mTickNanoTimeVec;//用一个vec存所有时间序列
    std::unordered_map<std::string,std::vector<int>> mTickIndexMap;
    std::unordered_map<std::string,int> mCurrentTickIndexMap;
    std::unordered_map<std::string,int> mCurrentGeneratedTickIndexMap;
    std::unordered_map<std::string,int> mCurrentOrderIndexMap;
    std::unordered_map<std::string,int> mCurrentTradeIndexMap;
    std::unordered_map<std::string,int> mCurrentQuoteIndexMap;
    std::unordered_map<std::string,SnapGenerator> mSnapGeneratorMap;
//    std::unordered_map<std::string,int> mCurrentStrategyIndex;
//    std::unordered_map<std::string ,std::list<MSQuoteData>> mStrategyQuoteDataListMap;
    std::unordered_map<std::string,std::unordered_map<long,LFL2OrderField>> mStrategyOrderMap;
    std::unordered_map<std::string,std::unordered_map<long,int>> mStrategyOrderIDMap;
    std::unordered_map<std::string,std::vector<LFL2TradeField>> mMatchedTradeDataMap;
    const TradeDataCollection*mTradeDataCollectionPtr;
    const OrderDataCollection*mOrderDataCollectionPtr;
    const BseSnapshotCollection*mBseSnapshotCollectionPtr;
    double* mMarketDataPtr;
    long mOrderDataSize;
    long mTradeDataSize;
    long mSnapshotSize;
    long long int mOrderIndex;
    long long int mTradeIndex;
    long long int mSnapshotIndex;
    long mSnapIndex;
    long mCurrentSeqNum;
    std::unordered_set<const char*,HashFunc,EqualFunc> mSHStockInstrumentUnorderedSet={"60","68"};
    std::unordered_set<const char*,HashFunc,EqualFunc> mSZStockInstrumentUnorderedSet={"00","30","31"};
    std::unordered_set<const char*,HashFunc,EqualFunc> mSZCBInstrumentUnorderedSet;
    std::unordered_set<const char*,HashFunc,EqualFunc> mSHCBInstrumentUnorderedSet;
    char mCurrentInstrumentID[31]{};
    LFL2TradeField mCurrentTradeData{};
    LFL2OrderField mCurrentOrderData{};
    MSQuoteData mCurrentQuoteData{};
    std::vector<std::string> mStockCodeVec;
    /*
     * 加入各种函数指针
     * */
    int mIntervalMs;
    BacktestEngine();



    BacktestEngine(const TradeDataCollection*tradeDataCollection,const OrderDataCollection*orderDataCollection,int interval_ms);
    BacktestEngine(const BseSnapshotCollection*bseSnapshotCollection,int interval_ms);


    bool is_done() const;
    void run();
    void next();

    void set_event_engine(EventEngine * event_engine);
    void set_strategy(IWCStrategy *strategy);
    void insert_market_data_event(const std::string& InstrumentID,long long nano_time);
    void insert_market_data_level2_event(const std::string& InstrumentID,long long nano_time);
    void insert_l2_order_event(const std::string& InstrumentID,long long nano_time,long long int seq_num=-1);
//    void insert_l2_quote_event(const std::string& InstrumentID,long long nano_time,long long int seq_num=-1);
    void insert_l2_trade_event(const std::string& InstrumentID,long long nano_time,long long int seq_num=-1);
//    void insert_modified_l2_quote_event(const std::string& InstrumentID,long long nano_time,long long int seq_num=-1,bool is_strategy=false);
//    void insert_modified_l2_trade_event(const std::string& InstrumentID,long long nano_time,long long int seq_num=-1);
    void on_market_data_level2_event(const std::string& InstrumentID,long long nano_time);
    void on_l2_order_event(const std::string& InstrumentID,long long nano_time,int rank);
//    void on_l2_quote_event(const std::string& InstrumentID,long long nano_time,int rank);
    void on_l2_trade_event(const std::string& InstrumentID,long long nano_time,int rank);
//    void on_l2_modified_quote_event(const std::string& InstrumentID,long long nano_time,int rank);
//    void on_l2_modified_trade_event(const std::string& InstrumentID,long long nano_time,int rank);
    void on_market_data_event(const std::string &InstrumentID,long long nano_time,int rank=-1);

    void on_insert_order_event(const std::string& InstrumentID,long long nano_time,int rank);

    void on_check_pending_event(int rank);

    void on_rtn_order_event(const std::string& InstrumentID,long long nano_time,int rank);

    void on_rtn_trade_event(const std::string& InstrumentID,long long nano_time,int rank);

    void on_cancel_order_event(const std::string& InstrumentID,long long nano_time,int rank);
    void call_check_pending();
    long long getNanoTime() ;
    void insert_callback(long long nano_time,std::function<void()>);

    int insert_limit_order(
            short source,const std::string &ticker_id, const std::string &exchange_id,
            double price, int volume, LfDirectionType direction, LfOffsetFlagType offset,long long nano_time);

    int cancel_order(short source, int request_id,long long nano_time);
    static long long addMillisecondsToLongTimestamp(long long timestamp, int milliseconds);


    static std::string formatTimestamp(long long timestamp);
    };
#endif //TEST_ORDER_BACKTEST_ENGINE_H
