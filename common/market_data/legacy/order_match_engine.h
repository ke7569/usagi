//
// Created by Administrator on 2024/1/2.
//
// Created by Administrator on 2024/1/2.
//

#ifndef MS_ORDER_MATCH_ENGINE_H
#define MS_ORDER_MATCH_ENGINE_H
#include <vector>
#include <list>
#include <map>
#include <array>
#include <algorithm> // For std::min

#include <unordered_map>
#include "common/contracts/legacy/LFDataStruct.h"
#include "common/contracts/legacy/RawDataStruct.h"
#include <stdexcept> // 包含标准异常类
class OrderMatchEngine{
public:
    OrderMatchEngine();
    void set_instrument_id_value(double instrument_id_value);
    void insert_quote_data(MSQuoteData&quote_data);
    void order_match(bool is_sell);
    void copy_snapshot_data(MSMarketData&market_data, const MSQuoteData&quote_data, double market_time_value) const;
    void clear();
public:
    std::map<int, int> mBidBook;
    std::map<int, int> mAskBook;
    std::unordered_map<long,int> mQuoteTagPriceMap;
private:
    void update_visible_side_from_book(bool is_bid);
    void update_summary_fields();
    void reset_snapshot_state();
    int mVolume;
    double mTurnover;
    double mLastPrice;
    MSMarketData mSnapshotState{};
    double mInstrumentIdValue{0.0};

};
#endif //MS_ORDER_MATCH_ENGINE_H
