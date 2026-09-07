//
// Created by admin on 2024/5/16.
//

#ifndef MYTHNIC_STOCK_TOTAL_HEADER_H
#define MYTHNIC_STOCK_TOTAL_HEADER_H
#include "adapters/deepwin/legacy/wc_strategy.h"
#include <functional>
#include <sstream>
using namespace std;

#define _epsilon 1e-6
#define ABS_UPPER_PRICE 1048576.0 // 2^20, 在缺少涨停价时作为最高价格使用
#ifndef T0_USE_DEEPWIN
#undef getNanoTime
#define getNanoTime() util->getNanoTime()
#undef KF_LOG_INFO
#define KF_LOG_INFO(logger,x) { std::ostringstream _kf_ob; _kf_ob << x; std::string _kf_oc = _kf_ob.str(); util->BackTestDW_C_kf_log("INFO", _kf_oc.c_str());}
#endif
#endif //MYTHNIC_STOCK_TOTAL_HEADER_H
