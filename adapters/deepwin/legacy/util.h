//
// Created by admin on 2023/11/13.
//

#ifndef STRATEGY_UTIL_H
#define STRATEGY_UTIL_H

#ifdef T0_USE_DEEPWIN

#include "WCStrategyUtil.h"
#include "WCDataWrapper.h"
#include "KfLog.h"

using kungfu::wingchun::WCStrategyUtil;
using kungfu::wingchun::WCStrategyUtilPtr;
using kungfu::wingchun::WCDataWrapper;
using kungfu::wingchun::WCDataWrapperPtr;
using kungfu::wingchun::BLCallback;
using kungfu::yijinjing::KfLog;
using kungfu::yijinjing::KfLogPtr;

#else

#include <memory>
#include "adapters/deepwin/legacy/backtest_engine.h"
#include <functional>
#include "adapters/deepwin/legacy/helper.h"
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#ifdef _WIN32
    #include <direct.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
#endif
class BacktestEngine;
class WCStrategyUtil{
public:
    BacktestEngine*mBacktestEngine;
    WCStrategyUtil();
    ~WCStrategyUtil();
    void subscribeMarketData(const std::vector<std::string>& tickers,short source);
    int req_position(short source);
    int req_account(short source);

    int insert_limit_order(
            short source, const std::string& ticker, const std::string& exchange_id,
            double price, int volume, char direction, char offset) const;
    int cancel_order(short source,int request_id) const;
    void set_backtest_engine(BacktestEngine* backtest_engine);
    void BackTestDW_C_kf_log(const std::string &l,const std::string &x) const;
    int insert_fok_order(
            short source, const std::string& ticker, const std::string& exchange_id,
            double price, int volume, char direction, char offset) const;
    int insert_fak_order(
            short source, const std::string& ticker, const std::string& exchange_id,
            double price, int volume, char direction, char offset) const;
    void insert_callback(long long,std::function<void()>&) const;
    long long getNanoTime() const;
    long long get_nano() const;
    void set_pos_flag(bool);
    static void set_log_strategy_name(const std::string& strategy_name);
private:
    static std::ofstream log_file_;
    static bool log_file_initialized_;
    static std::string log_strategy_name_;
    static void initialize_log_file();
};
class KfLog{


};
class WCDataWrapper{
public:
    void add_market_data(short source);
    void add_register_td(short source);


};
#ifndef KF_LOG_INFO
#define KF_LOG_INFO(l, x) { std::ostringstream _kf_ob; _kf_ob << x; std::string _kf_oc = _kf_ob.str(); util->BackTestDW_C_kf_log("INFO", _kf_oc.c_str());}
#endif
#ifndef KF_LOG_ERROR
#define KF_LOG_ERROR(l, x) { std::ostringstream _kf_ob; _kf_ob << x; std::string _kf_oc = _kf_ob.str(); util->BackTestDW_C_kf_log("ERROR", _kf_oc.c_str());}
#endif
#ifndef KF_LOG_DEBUG
#define KF_LOG_DEBUG(l, x) { std::ostringstream _kf_ob; _kf_ob << x; std::string _kf_oc = _kf_ob.str(); util->BackTestDW_C_kf_log("DEBUG", _kf_oc.c_str());}
#endif

typedef WCStrategyUtil* WCStrategyUtilPtr;
typedef KfLog* KfLogPtr;
typedef WCDataWrapper* WCDataWrapperPtr;
typedef std::function<void()> BLCallback;

#endif

#endif //STRATEGY_UTIL_H
