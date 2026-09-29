#include "sse/runtime/v06_strategy.h"
#include "common/oms/OrderLatency.h"
#include "common/oms/SseStockRules.h"
#include "sse/model/v06_audit.h"
#include "common/contracts/legacy/LFConstants.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace sse_v06 {
typedef nlohmann::json Json;
long long allowed_volume(bool buy,long long requested,long long position,long long reserved,int agreement,int lot) {
    if(lot<=0 || requested<=0)return 0;
    if(agreement==(buy?1:-1))return requested;
    const long long reduction=std::max(0LL,buy?-position:position);
    return std::min(requested,std::max(0LL,reduction-std::max(0LL,reserved))/lot*lot);
}
double offset_multiplier(unsigned hhmm) {
    const double values[]={10,5,4,3,2,1.8,1.6,1.4,1.2,1.1};
    return hhmm>=930 && hhmm<940 ? values[hhmm-930] : 1;
}
double bias_multiplier(unsigned t) {return t>=1430 && t<1500?1/.3:t>=1400&&t<1430?.7/.3:t>=1330&&t<1400?.5/.3:t>=1300&&t<1330?.4/.3:1;}
Strategy::Strategy(const Json& config,short source,const std::shared_ptr<StrategyExecution>& execution)
    : execution_(execution),source_(source),offset_(1),bias_(.3),baseline_(500000),limit_(1),
      buy_skew_(0),sell_skew_(0),next_sync_(0),signals_(0),global_ready_(false) {
    if(!execution || !execution->managed())throw std::runtime_error("v06 requires authoritative OMS");
    const Json& globals=config.at("global_params");
    offset_=globals.at("offset").get<double>();
    // v06 contract parameters are independent of the legacy strategy schedule.
    if(config.count("v06_strategy")) {
        const Json& v=config.at("v06_strategy");
        bias_=v.value("bias_factor",.3);baseline_=v.value("position_base_line",500000.0);limit_=v.value("position_limit_factor",1.0);
    }
    if(!std::isfinite(offset_)||offset_<=0||!std::isfinite(bias_)||bias_<=0||!std::isfinite(baseline_)||baseline_<=0||!std::isfinite(limit_)||limit_<0)
        throw std::runtime_error("invalid v06 strategy scales");
    const Json& inputs=config.at("ins_params");
    for(auto it=inputs.begin();it!=inputs.end();++it) {
        Stock stock;stock.bottom=it.value().at("static_position").get<long long>();
        stock.tradable=it.value().value("tradable",true) && !it.value().value("frozen",false);
        stock.open=it.value().value("Open",0.0);
        if(stock.bottom<0 || !std::isfinite(stock.open)||stock.open<0)throw std::runtime_error("invalid v06 holdings");
        stocks_[it.key().substr(0,6)]=stock;
    }
}
void Strategy::reference(const std::string& code,double open) {
    auto it=stocks_.find(code);
    if(it!=stocks_.end() && std::isfinite(open) && open>0 && it->second.open==0)it->second.open=open;
}
void Strategy::refresh(Stock& stock) {
    for(auto it=stock.pending.begin();it!=stock.pending.end();) {
        oms::OrderView view;
        if(!execution_->read_order(source_,it->first,&view))throw std::runtime_error("v06 OMS order unavailable");
        if(view.terminal) {
            if(view.priced_quantity!=view.filled)
                throw std::runtime_error("v06 fill amount coverage incomplete");
            stock.completed_fills+=view.filled;
            stock.net_completed+=(it->second.buy?1:-1)*double(view.known_amount)/oms::kMoneyScale;
            audit_text(Json{{"event","v06_order_terminal"},{"request_id",it->first},
                {"filled",view.filled},{"canceled",view.canceled},{"cancel_requested",it->second.cancel_requested}});
            it=stock.pending.erase(it);
        } else ++it;
    }
}
void Strategy::synchronize(std::uint64_t time) {
    if(time<next_sync_)return;
    next_sync_=(time/10000000ULL+1)*10000000ULL;
    double base=0,net=0;bool ready=true;unsigned missing=0;
    for(auto& item:stocks_) {
        Stock& stock=item.second;refresh(stock);oms::Position p;
        if(!execution_->read_t0_position(source_,item.first,"SSE",&p)){ready=false;continue;}
        if(!stock.fills_seeded) {
            if(!p.bought && !p.sold)stock.fills_seeded=true;
            else if(stock.pending.empty() && stock.completed_fills==0) {
                oms::Quantity filled=0;oms::Money amount=0;
                if(execution_->read_day_fills(source_,item.first,"SSE",&filled,&amount) && filled==p.bought+p.sold) {
                    stock.completed_fills=filled;stock.net_completed=double(amount)/oms::kMoneyScale;stock.fills_seeded=true;
                    audit_text(Json{{"event","v06_recovered_fills"},{"instrument",item.first},{"filled",filled},{"net_amount",amount}});
                }
            }
        }
        const long long initial=p.total-p.bought+p.sold;
        if((stock.bottom || initial) && stock.open<=0){ready=false;++missing;}
        base+=stock.bottom*stock.open;
        net+=(initial-stock.bottom)*stock.open+stock.net_completed;
        long long accounted=stock.completed_fills;
        for(const auto& order:stock.pending) {
            oms::OrderView view;
            if(!execution_->read_order(source_,order.first,&view)){ready=false;continue;}
            net+=(order.second.buy?1:-1)*double(view.known_amount)/oms::kMoneyScale;
            accounted+=view.filled;
            if(view.priced_quantity!=view.filled)ready=false;
        }
        // Do not invent fill prices on a restart with pre-existing intraday executions.
        if(accounted!=p.bought+p.sold)ready=false;
    }
    global_ready_=ready && base>0;
    const double exposure=base>0?net/base:0,u=std::min(std::fabs(exposure)/.05,1.0),penalty=u*u*.001;
    buy_skew_=exposure>0?penalty:0;sell_skew_=exposure<0?penalty:0;
    audit_text(Json{{"event","v06_global_skew"},{"time_us",time},{"ready",global_ready_},
        {"missing_open_prices",missing},{"base_value",base},{"net_value",net},{"exposure",exposure},
        {"buy_skew_bps",buy_skew_*10000},{"sell_skew_bps",sell_skew_*10000}});
}
void Strategy::on_signal(const std::string& code,const MSMarketDataField& view,const Heads& heads,std::uint64_t time,
                         std::uint64_t receive_ns,std::uint64_t signal_ns) {
    order_latency::Timing timing;timing.receive=receive_ns;timing.signal=signal_ns;timing.strategy=order_latency::now_ns();
    ++signals_;auto found=stocks_.find(code);if(found==stocks_.end())throw std::runtime_error("v06 unknown stock");
    Stock& stock=found->second;refresh(stock);synchronize(time);
    oms::Position p;if(!execution_->read_t0_position(source_,code,"SSE",&p))return;
    const long long relative=p.total-stock.bottom;
    const int consensus=agreement(heads);
    const int permission=time<34380000000ULL||!global_ready_||!stock.tradable?0:consensus;
    const double* v=view.ms_market_data.ms_market_data.data();
    const double bid=v[BidPrice1Index],ask=v[AskPrice1Index],mid=(bid+ask)/2;
    const double last=v[LastPriceIndex]>0?v[LastPriceIndex]:mid;
    const unsigned hhmm=unsigned(time/3600000000ULL*100+time/60000000ULL%60);
    const double offset=offset_*.001*offset_multiplier(hhmm),biasfactor=bias_*bias_multiplier(hhmm);
    const double bias=stock.bottom?std::max(-2.0,std::min(2.0,relative*biasfactor*last/baseline_)):(relative>=0?2:-2);
    const double theo=(1+heads[0]*.001)*mid,unit=offset*biasfactor*last/baseline_;
    const double hb=(1-bias*offset-offset-.0001-buy_skew_)*theo;
    const double hs=(1-bias*offset+offset-.0001+sell_skew_)*theo;
    long long remaining_buy=std::max(0LL,-relative),remaining_sell=std::max(0LL,relative);
    for(auto& item:stock.pending) {
        Pending& pending=item.second;oms::OrderView o;
        if(!execution_->read_order(source_,pending.id,&o))throw std::runtime_error("v06 order lookup failed");
        long long& reduction=pending.buy?remaining_buy:remaining_sell;
        const bool opening=o.working>reduction;
        reduction=std::max(0LL,reduction-static_cast<long long>(o.working));
        const bool invalid_opening=opening && permission!=(pending.buy?1:-1);
        const bool stale_quote=pending.quote && std::isfinite(heads[0]) &&
            (pending.buy?pending.price>hb:pending.price<hs);
        if((invalid_opening||stale_quote) && !pending.cancel_requested) {
            const int result=execution_->cancel(source_,pending.id);
            pending.cancel_requested=result>=0;
            audit_text(Json{{"event","v06_cancel_request"},{"instrument",code},{"request_id",pending.id},
                {"remaining",o.working},{"result",result},{"agreement",consensus}});
        }
    }
    if(!std::isfinite(heads[0])||bid<=0||ask<bid||time<34200000000ULL||time>=53820000000ULL||
       (time>=41400000000ULL && time<46800000000ULL))return;
    bool buy=true,quote=false;double price=0,requested=0;
    if(hb>ask){price=ask;requested=std::min((hb/ask-1)/unit,v[AskVolume1Index]);}
    else if(hs>0 && bid>hs){buy=false;price=bid;requested=std::min((bid/hs-1)/unit,v[BidVolume1Index]);}
    else if(bid+.01<=(1-bias*offset-offset*10-.0001-buy_skew_)*theo){quote=true;price=bid+.01;requested=v[AskVolume1Index];}
    else if(ask-.01>=(1-bias*offset+offset*10-.0001+sell_skew_)*theo){quote=true;buy=false;price=ask-.01;requested=v[BidVolume1Index];}
    if(!std::isfinite(requested)||requested<0||requested>std::numeric_limits<int>::max())return;
    const long long reserved=buy?p.working_buy:p.working_sell;
    const long long limit=time<34380000000ULL?0:static_cast<long long>(stock.bottom*limit_);
    const long long capacity=buy?limit-relative-p.working_buy:limit+relative-p.working_sell;
    // Sell capacity is remaining T+1 inventory, not a daily turnover cap.
    // PI capacity and agreement below still constrain exposure.
    const long long budget=buy?stock.bottom-p.bought-p.working_buy:
        static_cast<long long>(p.sellable-p.working_sell);
    const bool star=oms::is_sse_star(oms::Instrument{"SSE",code});
    const int lot=star?1:100;
    long long normal=std::max(0LL,std::min(static_cast<long long>(requested),std::min(capacity,budget)))/lot*lot;
    if(star)normal=std::min(normal,100000LL);
    long long allowed=allowed_volume(buy,normal,relative,reserved,permission,lot);
    if(star && !oms::star_quantity_valid(buy?oms::Side::Buy:oms::Side::Sell,allowed,p))allowed=0;
    audit_decision(code,time,heads,consensus,permission,relative,reserved,normal,allowed,buy,quote);
    // Existing single-flight semantics apply after cancellation decisions.
    if(allowed<(star?1:100)||price<=0||!stock.pending.empty()||execution_->has_working_order(code)||!execution_->permits_new_orders())return;
    const std::string signal_id = receive_ns && signal_ns ? order_latency::identity(code,signals_,timing) :
        "v06:"+code+":"+std::to_string(signals_);
    const int id=execution_->submit_managed(source_,code,"SSE",std::round(price*100)/100,static_cast<int>(allowed),
        buy?LF_CHAR_Buy:LF_CHAR_Sell,buy?LF_CHAR_Open:LF_CHAR_Close,
        quote?oms::OrderType::Limit:oms::OrderType::LimitThenCancel,quote?0:1000000000LL,std::function<bool()>(),signal_id);
    if(id>0)stock.pending.emplace(id,Pending{id,buy,quote,false,price});
}
}
