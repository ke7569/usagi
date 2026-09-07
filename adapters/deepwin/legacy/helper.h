#ifndef HELPER_HPP_
#define HELPER_HPP_
#include "third_party/csv/csv.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include "common/contracts/legacy/LFDataStruct.h"
#include <unordered_map>
#include <unordered_set>
#include <map>
#define LENGTH 2000000
//#define TRADE_TIME_LENGTH 12
//#define EXCHANGE_ID_LENGTH 4
//#define INSTRUMENT_ID_LENGTH 6
//#define ORDER_KIND_LENGTH 1
//#define ORDER_TYPE_LENGTH 1
//#define ORDER_BS_FLAG 1
//typedef  std::array<char,TRADE_TIME_LENGTH> time_struct;
//typedef  std::array<char,EXCHANGE_ID_LENGTH> exchangeID_struct;
//typedef  std::array<char,INSTRUMENT_ID_LENGTH> instrumentID_struct;
//typedef  std::array<char,ORDER_KIND_LENGTH> orderKind_struct;
//typedef  std::array<char,ORDER_TYPE_LENGTH> ordType_struct;
//typedef  std::array<char,ORDER_BS_FLAG> orderBSFlag_struct;

struct OrderData{
    std::vector<uint64_t>  orderTime;
    std::vector<int64_t>  exchangeID;
    std::vector<int64_t>  instrumentID;
    std::vector<double>  price;
    std::vector<double>  volume;
    std::vector<int64_t>   orderKind;
    std::vector<int64_t>   ordType;
    std::vector<int64_t> applSeqNum;
    std::vector<int64_t> bizIndex;
    std::vector<int64_t> orderNo;
    std::vector<int64_t> rank;
    std::vector<int64_t> isLast;
};
struct TradeData {
    std::vector<uint64_t>  tradeTime;
    std::vector<int64_t>  exchangeID;
    std::vector<int64_t>  instrumentID;
    std::vector<double>  price;
    std::vector<double>  volume;
    std::vector<int64_t>   orderKind;
    std::vector<int64_t>   orderBSFlag;
    std::vector<double>  turnover;
    std::vector<int64_t> bidApplSeqNum;
    std::vector<int64_t> offerApplSeqNum;
    std::vector<int64_t> applSeqNum;
    std::vector<int64_t> bizIndex;
    std::vector<int64_t> rank;
    std::vector<int64_t> isLast;
};


struct BseMarketData {
    std::vector<uint64_t> updateTime;        // 时间戳（纳秒）
    std::vector<std::string> instrumentID;
    std::vector<std::string> exchangeID;
    std::vector<double> lastPrice;
    std::vector<double> preClosePrice;
    std::vector<double> openPrice;
    std::vector<double> highestPrice;
    std::vector<double> lowestPrice;
    std::vector<int> volume;
    std::vector<double> turnover;
    std::vector<double> bidPrice1;
    std::vector<int> bidVolume1;
    std::vector<double> askPrice1;
    std::vector<int> askVolume1;
    std::vector<double> bidPrice2;
    std::vector<int> bidVolume2;
    std::vector<double> askPrice2;
    std::vector<int> askVolume2;
    std::vector<double> bidPrice3;
    std::vector<int> bidVolume3;
    std::vector<double> askPrice3;
    std::vector<int> askVolume3;
    std::vector<double> bidPrice4;
    std::vector<int> bidVolume4;
    std::vector<double> askPrice4;
    std::vector<int> askVolume4;
    std::vector<double> bidPrice5;
    std::vector<int> bidVolume5;
    std::vector<double> askPrice5;
    std::vector<int> askVolume5;
    std::vector<int64_t> rank;               // 用于排序的rank字段
};

//#include <longfist/LFDataStruct.h>
std::string rjust(const std::string& str, size_t length, char fillchar = ' ');
std::string  parseDateTime(long long  nano_time);
long long parseNanoTime(const std::string &str);
void get_csv_data(const char *filename,std::map<int,OrderData> &data);
void get_csv_data(const char *filename,std::map<int,TradeData> &data);
void get_bse_csv_data(const char *filename, std::map<int, BseMarketData> &data);
#endif
