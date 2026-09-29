#include "sse/model/v06_model.h"
#include "sse/factors/sse_tick_factors.h"
#include "third_party/nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cmath>
int main(int argc,char**argv){
    if(argc!=2)return 2;std::string root=argv[1];
    nlohmann::json cfg;std::ifstream meta(root+"/static.json");meta>>cfg;auto c=cfg.at("stock_day_parameters");
    sse_tick::DailyStaticMetadata m;m.date=20260401;m.has_date=true;m.free_share=c.at("free_share");m.has_free_share=true;
    m.avg_amount=c.at("avg_amount");m.has_avg_amount=true;m.turnover_threshold=c.at("turnover_threshold");m.has_turnover_threshold=true;
    m.pre_close=c.at("pre_close");m.has_pre_close=true;m.limit_price=c.at("limit_price");m.has_limit_price=true;m.stop_price=c.at("stop_price");m.has_stop_price=true;
    sse_tick::OrderBook book("600000");sse_tick::FactorState factors;factors.set_static_metadata(m);
    factors.set_v06(true);
    std::ifstream events(root+"/raw-events.txt"),cuts(root+"/checkpoints.txt"),gold(root+"/golden-factors.f32",std::ios::binary);
    std::ofstream output(root+"/actual-factors.f32",std::ios::binary);
    sse_live::TickEvent e={};e.security_id="600000";e.channel_no=5;
    auto next=[&](){return bool(events>>e.tick_index>>e.event_type>>e.time_of_day_micros>>e.price_raw>>e.quantity_raw>>e.buy_order_no>>e.sell_order_no>>e.side);};
    bool have=next();std::uint64_t seq,time;int index;double maxerr[50]={};unsigned fail[50]={};unsigned rows=0,rejected=0;
    while(cuts>>seq>>time>>index){
        while(have&&e.tick_index<=seq){e.amount_raw=0;e.side=e.side=='B'?0:1;auto r=book.apply(e);if(!r.accepted)++rejected;have=next();}
        auto row=factors.build(book,time);
        if(index<0)continue;
        const auto actual=sse_v06::from_legacy_factors(row.values);float expected[50];gold.read(reinterpret_cast<char*>(expected),200);
        output.write(reinterpret_cast<const char*>(actual.data()),200);
        for(unsigned i=0;i<50;++i){const double err=std::fabs(actual[i]-expected[i]);maxerr[i]=std::max(maxerr[i],err);if(err>2e-5+2e-5*std::fabs(expected[i]))++fail[i];}
        ++rows;
    }
    std::cout<<"rows="<<rows<<" rejected_raw_events="<<rejected<<'\n';unsigned bad=0;
    for(unsigned i=0;i<50;++i){bad+=fail[i];std::cout<<i<<' '<<sse_v06::factor_name(i)<<" max_error="<<maxerr[i]<<" mismatched_rows="<<fail[i]<<'\n';}
    return bad?1:0;
}
