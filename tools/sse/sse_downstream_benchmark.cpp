// Controlled component benchmark, not a live latency or complete-book replay.
#include "sse/market_data/sse_tick_order_book.h"
#include "sse/factors/sse_tick_factors.h"
#include "sse/model/sse_hybrid_model.h"
#include "third_party/nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sched.h>
#include <stdexcept>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

typedef std::uint64_t U64;
typedef nlohmann::json Json;
volatile double sink = 0;
U64 wall() { timespec t; if(clock_gettime(CLOCK_MONOTONIC_RAW,&t))throw std::runtime_error("clock");return U64(t.tv_sec)*1000000000ULL+t.tv_nsec; }
U64 begin() { _mm_lfence(); const U64 t=__rdtsc();_mm_lfence();return t; }
U64 end() { unsigned aux;const U64 t=__rdtscp(&aux);_mm_lfence();return t; }
double rate = 0;
U64 ns(U64 d) { return U64(d/rate); }
Json stats(std::vector<U64> values) {
    std::sort(values.begin(),values.end());
    Json out;out["count"]=values.size();
    const char* names[]={"min_us","p50_us","p90_us","p99_us","max_us"};
    const double qs[]={0,.5,.9,.99,1};
    for(unsigned i=0;i<5;++i){const double pos=qs[i]*(values.size()-1);const std::size_t a=std::size_t(pos),b=std::min(a+1,values.size()-1);out[names[i]]=(values[a]+(values[b]-values[a])*(pos-a))/1000.;}
    out["mean_us"]=std::accumulate(values.begin(),values.end(),0.)/values.size()/1000.;return out;
}
sse_tick::DailyStaticMetadata metadata() {
    sse_tick::DailyStaticMetadata m;m.date=20260908;m.avg_amount=8000000.;m.turnover_threshold=1000.;m.free_share=10000000.;
    m.pre_close=10.;m.limit_price=11.;m.stop_price=9.;m.has_date=m.has_avg_amount=m.has_turnover_threshold=m.has_free_share=m.has_pre_close=m.has_limit_price=m.has_stop_price=true;return m;
}
struct Fixture {
    sse_tick::OrderBook book;
    sse_tick::FactorState factors;
    sse_hybrid_model::State model_state;
    U64 sequence,order,exchange;
    Fixture(unsigned count) : book("600000"),sequence(0),order(count+1),exchange(36000000000ULL) {
        factors.set_static_metadata(metadata());
        for(unsigned i=0;i<count;++i){
            sse_live::TickEvent t=sse_live::TickEvent();t.security_id="600000";t.channel_no=1;t.tick_index=++sequence;t.provider_sequence=sequence;t.app_seq_num=sequence;
            t.time_of_day_micros=exchange-60000000ULL+U64(i)*60000000ULL/count;t.event_type='A';t.side=i%2;
            const unsigned level=(i/2)%50;t.price_raw=t.side?10010+level*10:10000-level*10;t.quantity_raw=1000000;
            if(t.side)t.sell_order_no=i+1;else t.buy_order_no=i+1;
            if(!book.apply(t).accepted)throw std::runtime_error("seed book");
        }
        book.take_flow_window();factors.build(book,exchange);
    }
    std::vector<sse_live::TickEvent> make(unsigned count) {
        if(count%5)throw std::runtime_error("five-event cycles required");
        std::vector<sse_live::TickEvent> result;result.reserve(count);exchange+=1000;
        for(unsigned j=0;j<count;j+=5){
            const U64 buy=order++,sell=order++;
            for(unsigned k=0;k<5;++k){
                sse_live::TickEvent t=sse_live::TickEvent();t.security_id="600000";t.channel_no=1;t.tick_index=++sequence;t.provider_sequence=sequence;t.app_seq_num=sequence;
                t.time_of_day_micros=exchange;t.buy_order_no=buy;t.sell_order_no=sell;
                t.event_type=k<2?'A':k==2?'T':'D';t.side=(k==1||k==4)?1:0;t.price_raw=k==2?10005:t.side?10010:10000;
                t.quantity_raw=k<2?1000000:k==2?500000:0;result.push_back(t);
            }
        }
        return result;
    }
};
int main(int argc,char**argv){try{
    if(argc!=3)throw std::runtime_error("usage PROFILE CPU");
    const int cpu=std::stoi(argv[2]);if(cpu<0||cpu>=CPU_SETSIZE)throw std::runtime_error("CPU");
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);if(sched_setaffinity(0,sizeof(set),&set))throw std::runtime_error("affinity");
    Json profile;std::ifstream input(argv[1]);input>>profile;const Json& p=profile.at("prediction");
    sse_hybrid_model::Model model;std::string error;
    if(!model.load(p.at("model_path").get<std::string>(),p.at("snapshot_baseline_model_path").get<std::string>(),p.at("snapshot_baseline_scaler_path").get<std::string>(),p.at("snapshot_auction59_model_path").get<std::string>(),p.at("snapshot_auction59_scaler_path").get<std::string>(),&error))throw std::runtime_error(error);
    const U64 w0=wall(),t0=begin();usleep(50000);const U64 t1=end(),w1=wall();rate=double(t1-t0)/(w1-w0);
    Json report;report["cpu"]=sched_getcpu();report["tsc_cycles_per_ns"]=rate;report["model_path"]=p.at("model_path");
    report["case_definition"]="Synthetic one-stock book with 50 bid and 50 ask price levels, half the orders approximately younger than 30 seconds. Each five-event cycle adds a buy and sell, partially trades both, then fully cancels both residuals. Resting order count is preserved. All events must be accepted. Input construction and fixture seeding excluded.";
    report["cases"]=Json::array();
    const unsigned counts[]={1000,10000,50000},flows[]={20,520};
    for(unsigned n:counts)for(unsigned flow:flows){
        Fixture f(n);std::vector<U64> updates,factors,inference,total;for(auto*v:{&updates,&factors,&inference,&total})v->reserve(200);
        for(unsigned iteration=0;iteration<220;++iteration){
            const auto events=f.make(flow);U64 accepted=0;
            const U64 a=begin();for(const auto&t:events)accepted+=f.book.apply(t).accepted;const U64 b=end();
            const U64 c=begin();const sse_tick::FactorRow row=f.factors.build(f.book,f.exchange);const U64 d=end();
            sse_hybrid_model::Prediction pred;std::string model_error;
            const U64 e=begin();const bool ok=model.on_tick(row.values,"sse",f.exchange,&f.model_state,&pred,&model_error);const U64 g=end();
            if(accepted!=flow||f.book.live_order_count()!=n||!row.validity.complete||!ok||!pred.tick_generated||!std::isfinite(pred.tick_pred))throw std::runtime_error("case validity: "+model_error);
            sink+=pred.tick_pred+row.values[49];
            if(iteration>=20){updates.push_back(ns(b-a));factors.push_back(ns(d-c));inference.push_back(ns(g-e));total.push_back(ns(g-a));}
        }
        std::vector<U64> depth;std::vector<sse_tick::Level>bids,asks;
        for(unsigned i=0;i<120;++i){const U64 a=begin();f.book.full_depth('B',f.exchange,&bids);f.book.full_depth('S',f.exchange,&asks);const U64 b=end();if(i>=20)depth.push_back(ns(b-a));sink+=bids.size()+asks.size();}
        Json row;row["resting_orders"]=n;row["total_price_levels"]=100;row["records_before_sample"]=flow;
        row["book_updates"]=stats(updates);row["factor_build"]=stats(factors);row["tick_inference"]=stats(inference);row["combined"]=stats(total);row["two_sided_full_depth_only"]=stats(depth);
        report["cases"].push_back(row);
    }
    Fixture f(10000);std::map<char,std::vector<U64>> operation;for(char c:{'A','D','T'})operation[c].reserve(4000);
    for(unsigned iteration=0;iteration<220;++iteration){const auto events=f.make(20);for(const auto&t:events){const U64 a=begin();const auto r=f.book.apply(t);const U64 b=end();if(!r.accepted)throw std::runtime_error("operation");if(iteration>=20)operation[t.event_type].push_back(ns(b-a));}f.book.take_flow_window();}
    for(const auto& item:operation)report["book_operation_at_10000_orders"][std::string(1,item.first)]=stats(item.second);
    report["checksum"]=sink;std::cout<<report.dump(2)<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
