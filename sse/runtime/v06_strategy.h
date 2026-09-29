#ifndef SSE_V06_STRATEGY_H
#define SSE_V06_STRATEGY_H
#include "sse/model/v06_model.h"
#include "common/contracts/StrategyExecution.h"
#include "common/contracts/legacy/RawDataStruct.h"
#include "third_party/nlohmann/json.hpp"
#include <map>
#include <memory>
namespace sse_v06 {
long long allowed_volume(bool buy,long long requested,long long relative,long long reserved,int consensus,int lot=100);
double offset_multiplier(unsigned hhmm);
double bias_multiplier(unsigned hhmm);
class Strategy {
public:
    Strategy(const nlohmann::json& config,short source,const std::shared_ptr<StrategyExecution>& execution);
    void reference(const std::string& code,double open_price);
    void on_signal(const std::string& code,const MSMarketDataField& view,const Heads& heads,std::uint64_t time_us,
                   std::uint64_t receive_ns=0,std::uint64_t signal_ns=0);
    std::uint64_t signals() const { return signals_; }
private:
    struct Pending { int id; bool buy,quote,cancel_requested; double price; };
    struct Stock {
        long long bottom;
        double open,net_completed;
        long long completed_fills;
        bool tradable,fills_seeded;
        std::map<int,Pending> pending;
        Stock():bottom(0),open(0),net_completed(0),completed_fills(0),tradable(true),fills_seeded(false){}
    };
    void refresh(Stock& stock);
    void synchronize(std::uint64_t time_us);
    std::map<std::string,Stock> stocks_;
    std::shared_ptr<StrategyExecution> execution_;
    short source_;
    double offset_,bias_,baseline_,limit_,buy_skew_,sell_skew_;
    std::uint64_t next_sync_,signals_;
    bool global_ready_;
};
}
#endif
