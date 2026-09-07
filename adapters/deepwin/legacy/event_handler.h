//
// Created by admin on 2023/11/3.
//

#ifndef TEST_ORDER_EVENT_HANDLER_H
#define TEST_ORDER_EVENT_HANDLER_H
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <iostream>
#include <list>
#include <set>
#include <queue>
//#include "backtest_engine.cpp"

//
// Created by admin on 2023/11/2.
//
#include "adapters/deepwin/legacy/backtest_engine.h"
#include <cstring>
#include <utility>
#include <vector>
enum EventType {
    EVENT_MARKET,
    EVENT_MARKET_LEVEL2,
    EVENT_L2_ORDER,
    EVENT_L2_QUOTE,
    EVENT_L2_MODIFIED_QUOTE,
    EVENT_L2_TRADE,
    EVENT_L2_MODIFIED_TRADE,
    EVENT_CANCEL_ORDER,
    EVENT_INSERT_ORDER,
    EVENT_ORDER,
    EVENT_TRADE,
    EVENT_TIMER,
    EVENT_NUM
};

//std::array<EventType,EVENT_NUM> event_array;
struct WeightedItem {

    EventType value;
    std::string instrument_id;
    int weight;
    int order;
    long long int seq_num;

    WeightedItem(EventType v,std::string  i, int w, int o,long long int s=-1) : value(v),instrument_id(std::move(i)), weight(w), order(o),seq_num(s) {}
};

struct CompareWeight {
    bool operator()(WeightedItem const& item1, WeightedItem const& item2) {
        // 如果权重相同，根据插入顺序排序
        if (item1.weight == item2.weight) {
            if((item1.seq_num!=-1)&&(item2.seq_num!=-1)&&(item1.seq_num!=item2.seq_num)){
                return item1.seq_num<item2.seq_num;
            }else{
                return item1.order < item2.order;
            }
        }
        // 否则，更小的权重在前
        return item1.weight < item2.weight;
    }
};
class BacktestEngine;

class EventEngine{
public:
//    std::mutex mMtx;
//    std::condition_variable mCv;
    BacktestEngine *mBacktestEngine;
    long long mCurrentNanoTime;
    int mRank;
    std::map<long long, std::set<WeightedItem, CompareWeight>> mEventMap;//
    EventEngine();
    void set_backtest_engine(BacktestEngine *backtest_engine);
    void run();
    void run_ready();
    void registerHandler();
    void set_event(long long nano_time,const std::string& InstrumentID,EventType event_type,long long int seq_num=-1);
};
#endif //TEST_ORDER_EVENT_HANDLER_H
