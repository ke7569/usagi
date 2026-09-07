//
// Created by Administrator on 2024/1/15.
//

#ifndef RAW_DATA_GENERATOR_RAWDATASTRUCTAPI_H
#define RAW_DATA_GENERATOR_RAWDATASTRUCTAPI_H
#include "common/contracts/legacy/RawDataStruct.h"
#include "adapters/deepwin/legacy/backtest_engine.h"
#ifdef __cplusplus
extern "C" {
#endif


void* CreateOrderDataCollection(const uint64_t *orderTime,
                                const int64_t *exchangeID,
                                const int64_t *instrumentID,
                                const double *price,
                                const double *volume,
                                const int64_t *orderKind,
                                const int64_t *applSeqNum,
                                const int64_t *ordType,
                                const int64_t *orderNo,
                                const int64_t *bizIndex,
                                const int64_t *isLast,
                                const int64_t *rank,
                                int64_t orderLength);
void DestroyOrderDataCollection(void* collection);

// 创建 TradeDataCollection 实例的函数
void* CreateTradeDataCollection(const uint64_t *tradeTime,
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




// 销毁 TradeDataCollection 实例
void DestroyTradeDataCollection(void* collection);

// 创建 BseSnapshotCollection 实例的函数
void* CreateBseSnapshotCollection(const uint64_t *updateTime,
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

// 销毁 BseSnapshotCollection 实例
void DestroyBseSnapshotCollection(void* collection);

// 创建SnapGenerator实例的函数
void* CreateBacktestEngine(void* tradeDataCollection, void* orderDataCollection,int interval_ms);
void* CreateBacktestEngineForBseSnapshot(void* bseSnapshotCollection, int interval_ms);

// 运行SnapGenerator实例的函数
void BacktestEngineRun(void* instance);

void BacktestEngineSetStrategy(void *backtestEngineInstance,void *strategyInstance);
void BacktestEngineSetEventEngine(void *backtestEngineInstance,void *eventEngineInstance);
void StrategySetWCStrategyUtil(void *strategyInstance,void* wcStrategyUtilInstance);
size_t GetBasicFieldNum();


// 销毁BacktestEngine实例的函数
void DestroyBacktestEngine(void* instance);

void* CreateStrategy(const char* name, const char* config_json_str);
void StrategyInit(void* instance);
// ...其他成员函数的C接口声明...
void DestroyStrategy(void* instance);

void* CreateEventEngine();

void EventEngineSetBacktestEngine(void* eventEngineInstance, void* backtestEngineInstance);

void DestroyEventEngine(void* eventEngineInstance);

void* CreateWCStrategyUtil();
void WCStrategyUtilSetBacktestEngine(void* wcStrategyUtilInstance, void* backtestEngineInstance);
void DestroyWCStrategyUtil(void* wcStrategyUtilInstance);

#ifdef __cplusplus
}
#endif

#endif //RAW_DATA_GENERATOR_RAWDATASTRUCTAPI_H
