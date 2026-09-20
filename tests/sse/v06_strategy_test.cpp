#include "sse/runtime/v06_strategy.h"
#include "common/oms/OrderLatency.h"
#include "common/contracts/legacy/LFConstants.h"
#include <cassert>
#include <limits>
#include <iostream>
using nlohmann::json;
struct Fake : StrategyExecution {
    std::string signal_id;
    bool recovery_available=false;oms::Quantity recovery_qty=0;oms::Money recovery_amount=0;
    bool read_day_fills(short,const std::string&,const std::string&,oms::Quantity*q,oms::Money*a)const override{if(!recovery_available)return false;*q=recovery_qty;*a=recovery_amount;return true;}

    oms::Position position;std::map<int,oms::OrderView> orders;int created=0,cancels=0;
    bool managed() const override{return true;}
    bool permits_new_orders() const override{return true;}
    long long now_ns() const override{return 1;}
    bool read_position(short,const std::string&,const std::string&,oms::Position* p)const override{*p=position;return true;}
    bool read_order(short,int id,oms::OrderView* o)const override{auto i=orders.find(id);if(i==orders.end())return false;*o=i->second;return true;}
    bool has_working_order(const std::string&)const override{return position.working_buy||position.working_sell;}
    int submit_limit(short,const std::string&,const std::string&,double,int,char,char)override{return -1;}
    int submit_managed(short,const std::string&,const std::string&,double price,int volume,char direction,char,
        oms::OrderType type,long long delay,const std::function<bool()>&,const std::string& signal)override{
        signal_id=signal;
        assert(type==oms::OrderType::Limit || delay==1000000000LL);
        const int id=++created;oms::OrderView o;o.owned=true;o.working=volume;
        o.command.intent.side=direction==LF_CHAR_Buy?oms::Side::Buy:oms::Side::Sell;
        o.command.intent.quantity=volume;o.command.intent.price=price*oms::kMoneyScale;
        orders[id]=o;if(direction==LF_CHAR_Buy)position.working_buy+=volume;else position.working_sell+=volume;
        return id;
    }
    int cancel(short,int id)override{++cancels;return id;}
    bool schedule_cancel(short,int,int)override{return true;}
};
json config(bool tradable=true){return json{{"global_params",{{"offset",1.0}}},{"ins_params",{{"600000.SH",{{"static_position",1000},{"tradable",tradable}}}}}};}
MSMarketDataField view(){MSMarketDataField v={MSMarketData()};auto& a=v.ms_market_data.ms_market_data;
    a[BidPrice1Index]=10;a[AskPrice1Index]=10.01;a[LastPriceIndex]=10;
    a[AskVolume1Index]=500;a[BidVolume1Index]=500;return v;}
int main(){
    using namespace sse_v06;
    assert(allowed_volume(false,500,300,100,0)==200);
    assert(allowed_volume(true,600,-250,0,0)==200);
    assert(allowed_volume(true,600,-250,0,1)==600);
    assert(allowed_volume(true,500,300,0,0)==0);
    assert(offset_multiplier(930)==10&&offset_multiplier(933)==3&&offset_multiplier(940)==1);
    assert(bias_multiplier(1300)==.4/.3&&bias_multiplier(1430)==1/.3);
    Heads up={{100,1,2,3}},mixed={{100,-1,2,3}},down={{-100,-1,-2,-3}};
    // An afternoon restart can restore priced morning executions; unknown or
    // mismatched coverage must continue blocking new opening exposure.
    for(int mode=0;mode<3;++mode){auto restored=std::make_shared<Fake>();restored->position.total=1000;restored->position.sellable=900;restored->position.bought=100;restored->recovery_available=mode!=0;restored->recovery_qty=mode==2?99:100;restored->recovery_amount=10000000;
        Strategy resumed(config(),1,restored);resumed.reference("600000",10);resumed.on_signal("600000",view(),up,34380000000ULL);assert(restored->created==(mode==1?1:0));}
    // Zero starting holdings: the configured baseline determines the entire
    // buy budget. Bought shares never become today's sellable inventory.
    for(const auto& code : {std::string("600058"),std::string("688327")}) {
        for(int bottom : {200,400}) {
            auto zero=std::make_shared<Fake>();
            json cfg={{"global_params",{{"offset",1.0}}},{"ins_params",{{code+".SH",{{"static_position",bottom}}}}}};
            Strategy replenishing(cfg,1,zero);replenishing.reference(code,10);
            replenishing.on_signal(code,view(),down,34380000000ULL);
            assert(zero->created==0); // PI=-bottom, no sellable shares.
            const auto signal_ns=order_latency::now_ns();
            replenishing.on_signal(code,view(),up,34380000001ULL,signal_ns-1000000,signal_ns);
            assert(zero->created==1 && zero->position.working_buy==bottom);
            order_latency::Timing trace;
            assert(order_latency::parse(zero->signal_id,&trace));
            assert(trace.receive==signal_ns-1000000 && trace.signal==signal_ns && trace.strategy>=signal_ns);
            replenishing.on_signal(code,view(),up,34380000002ULL);
            assert(zero->created==1); // The live order reserves the budget.
            auto& order=zero->orders[1];order.terminal=true;order.filled=bottom;
            order.priced_quantity=bottom;order.known_amount=bottom*order.command.intent.price;order.working=0;
            zero->position.total=bottom;zero->position.bought=bottom;zero->position.working_buy=0;
            assert(zero->position.sellable==0);
            replenishing.on_signal(code,view(),up,34380000003ULL);
            replenishing.on_signal(code,view(),down,34380000004ULL);
            assert(zero->created==1); // PI=0: no remaining buys and no sells.
        }
    }
    // 600596: initial 400, sell 200 then buy 200 -> total 400, PI +200.
    // A previous sale must not prevent selling the remaining old inventory.
    for(int available : {0,100,200}) {
        auto x=std::make_shared<Fake>();x->position.total=400;x->position.sellable=available;
        x->position.bought=200;x->position.sold=200;
        x->recovery_available=true;x->recovery_qty=400;x->recovery_amount=0;
        json cfg=config();cfg["ins_params"]["600000.SH"]["static_position"]=200;
        Strategy st(cfg,1,x);st.reference("600000",10);
        Heads reduce={{-100,1,2,3}}; // Mixed heads allow PI reduction only.
        st.on_signal("600000",view(),reduce,36000000000ULL);
        assert(x->created==(available>0?1:0));assert(x->position.working_sell==available);
        if(available!=200)continue;
        st.on_signal("600000",view(),reduce,36000000001ULL);
        assert(x->created==1 && x->position.working_sell==200);
        // Partial fill reduces both actual holdings and sellable exactly once.
        auto& o=x->orders[1];o.working=100;o.filled=100;o.priced_quantity=100;
        o.known_amount=100*o.command.intent.price;
        x->position.total=300;x->position.sellable=100;x->position.sold=300;x->position.working_sell=100;
        st.on_signal("600000",view(),reduce,36000000002ULL);
        assert(x->created==1 && x->position.working_sell==100);
        // Acknowledged cancellation releases only the unfilled reservation.
        o.terminal=true;o.working=0;o.canceled=100;x->position.working_sell=0;
        st.on_signal("600000",view(),reduce,36000000003ULL);
        assert(x->created==2 && x->position.working_sell==100);
        auto& last=x->orders[2];last.terminal=true;last.working=0;last.filled=100;last.priced_quantity=100;
        last.known_amount=100*last.command.intent.price;
        x->position.total=200;x->position.sellable=0;x->position.sold=400;x->position.working_sell=0;
        st.on_signal("600000",view(),down,36000000004ULL);
        assert(x->created==2); // Today's purchases remain unsellable.
    }
    auto f=std::make_shared<Fake>();f->position.total=1000;f->position.sellable=1000;
    Strategy s(config(),1,f);s.reference("600000",10);
    s.on_signal("600000",view(),up,34200000000ULL);assert(f->created==0);
    s.on_signal("600000",view(),up,34380000000ULL);assert(f->created==1&&f->position.working_buy==500);
    s.on_signal("600000",view(),mixed,34380000001ULL);assert(f->cancels==1&&f->position.working_buy==500);
    // A partial fill before cancel confirmation changes actual holdings and remaining reservation.
    f->position.total+=100;f->position.bought=100;f->position.working_buy=400;
    f->orders[1].working=400;f->orders[1].filled=100;f->orders[1].known_amount=10000000;
    f->orders[1].priced_quantity=100;
    s.on_signal("600000",view(),mixed,34380000002ULL);assert(f->created==1&&f->position.working_buy==400&&f->cancels==1);
    f->orders[1].terminal=true;f->orders[1].working=0;f->orders[1].canceled=400;f->position.working_buy=0;
    s.on_signal("600000",view(),down,34380000003ULL);assert(f->created==2);
    // Missing open price forbids expansion but is not a reason to skip model signals.
    auto g=std::make_shared<Fake>();g->position.total=1000;g->position.sellable=1000;
    Strategy missing(config(),1,g);missing.on_signal("600000",view(),up,34380000000ULL);assert(g->created==0);
    auto frozen=std::make_shared<Fake>();frozen->position.total=1000;frozen->position.sellable=1000;
    Strategy frozen_s(config(false),1,frozen);frozen_s.reference("600000",10);frozen_s.on_signal("600000",view(),up,34380000000ULL);assert(frozen->created==0);
    // Disagreement must still allow an existing negative deviation to be reduced before 09:33.
    auto close=std::make_shared<Fake>();close->position.total=700;close->position.sellable=700;
    Strategy closing(config(),1,close);closing.reference("600000",10);
    closing.on_signal("600000",view(),mixed,34260000000ULL);assert(close->created==1&&close->position.working_buy==300);
    // STAR minimums must not round up exposure. Above 200, use single shares.
    for (int quantity : {100,199,200,201,250}) {
        auto star=std::make_shared<Fake>();star->position.total=1000;star->position.sellable=1000;
        json cfg=config();cfg["ins_params"]["688327.SH"]=cfg["ins_params"]["600000.SH"];cfg["ins_params"].erase("600000.SH");
        Strategy st(cfg,1,star);st.reference("688327",10);
        auto book=view();book.ms_market_data.ms_market_data[AskVolume1Index]=quantity;
        st.on_signal("688327",book,up,34380000000ULL);
        assert(star->created==(quantity>=200?1:0));
        assert(star->position.working_buy==(quantity>=200?quantity:0));
    }
    // A canceled partial fill can leave a sub-200 sellable remainder. Sell all,
    // never a smaller fragment, and never use the exception for buying.
    for(int requested : {99,100,199}) {
        auto star=std::make_shared<Fake>();star->position.total=100;star->position.sellable=100;
        json cfg=config();cfg["ins_params"]["688327.SH"]=cfg["ins_params"]["600000.SH"];cfg["ins_params"].erase("600000.SH");
        Strategy st(cfg,1,star);st.reference("688327",10);
        auto book=view();book.ms_market_data.ms_market_data[BidVolume1Index]=requested;
        st.on_signal("688327",book,down,34380000000ULL);
        assert(star->created==(requested>=100?1:0));
        assert(star->position.working_sell==(requested>=100?100:0));
    }
    std::cout<<"v06 strategy gates, pending cancellation, partial fills, frozen/open-price guards passed\n";
}
