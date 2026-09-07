//
// Created by admin on 2023/11/7.
//

#ifndef BACKTEST_ENGINE_WC_STRATEGY_H
#define BACKTEST_ENGINE_WC_STRATEGY_H

#ifdef T0_USE_DEEPWIN

#include "IWCStrategy.h"

using kungfu::wingchun::IWCStrategy;

#else

#include "adapters/deepwin/legacy/backtest_engine.h"
#include "adapters/deepwin/legacy/util.h"
#include "adapters/deepwin/legacy/helper.h"
#include <functional>

namespace kungfu {
namespace yijinjing {
class IControlCenter;
}
}

class BacktestEngine;
class WCStrategyUtil;
class KfLog;
class WCDataWrapper;
class IWCStrategy{
public:
    virtual ~IWCStrategy() = default;

    BacktestEngine *mBacktestEngine;
    WCStrategyUtil* util;
    KfLog *logger;
    WCDataWrapper * data;
    const std::string mName;
    explicit IWCStrategy(std::string name);
    virtual void init()=0;
    virtual void on_rtn_trade(const struct LFRtnTradeField *data, int request_id, short source, long rcv_time)=0;
    virtual void on_l2_trade(const struct LFL2TradeField *data, short source, long rcv_time)=0;
//    virtual void on_l2_modified_trade(const struct LFL2TradeField *data, short source, long rcv_time)=0;
    virtual void on_market_data(const struct LFMarketDataField *mds, short source, long rcv_time)=0;
    virtual void on_ms_market_data(const MSMarketDataField *mds, short source, long rcv_time)=0;
//    virtual void on_modified_market_data(const struct LFMarketDataField *mds, short source, long rcv_time)=0;
    virtual void on_market_data_level2(const struct LFL2MarketDataField *mds, short source, long rcv_time)=0;
    virtual void on_l2_order(const struct LFL2OrderField *data, short source, long rcv_time)=0;
//    virtual void on_l2_quote(const struct QuoteData*data,short source,long rcv_time)=0;
//    virtual void on_l2_modified_quote(const struct QuoteData*data,short source,long rcv_time)=0;
//    virtual void on_l2_modified_order(const struct LFL2OrderField *data, short source, long rcv_time)=0;
    //    virtual void check_pending_order()=0;
    virtual void on_rtn_order(const struct LFRtnOrderField *data, int request_id, short source, long rcv_time)=0;
    virtual void on_rsp_account(const LFRspAccountField* data, int request_id, short source, long rcv_time,
        int errorId = 0, const char* errorMsg = nullptr);
    virtual void on_rtn_pos_option(const LFRspPositionField* data, bool isLast, int request_id, short source, long rcv_time);
    virtual void set_cc(kungfu::yijinjing::IControlCenter* cc);
    virtual void start();
    int insert_limit_order(
            short source, const std::string &ticker, const std::string &exchange_id,
            double price, int volume, char direction, char offset) const;
    int cancel_order(short source,int request_id) const;
    int insert_fok_order(
            short source, const std::string &ticker, const std::string &exchange_id,
            double price, int volume, char direction, char offset) const;
    int insert_fak_order(
            short source, const std::string &ticker, const std::string &exchange_id,
            double price, int volume, char direction, char offset) const;
    int req_position(short source) const;
    int req_account(short source) const;
    void insert_callback(long long,std::function<void()>&) const;
    void set_strategy_util(WCStrategyUtil* wc_util);
    void BackTestDW_C_kf_log(const std::string &l,const std::string &x) const;

};

#endif

#endif //BACKTEST_ENGINE_WC_STRATEGY_H
