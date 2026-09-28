#include "sze/sampling/mix153060_runtime.h"
#include <cassert>
#include <iostream>
using namespace mix153060;
StaticInputs inputs() {
    StaticInputs i;i.instrument="000710";i.trading_date=20260928;i.v06_baseline=true;
    i.average_amount=8000;i.turnover_threshold=1;i.free_share=100000;
    i.pre_close=10;i.upper_limit=12;i.lower_limit=8;
    return i;
}
void order(Runtime& r,long long id,bool buy,double price,long long qty,OrderKind k=OrderKind::kLimit) {
    OrderEvent o;o.app_sequence=id;o.exchange_time_us=o.local_time_us=34200000000LL+id*110000000;
    o.buy=buy;o.price=price;o.volume=qty;o.kind=k;SampleBuffer b;r.on_order(o,&b);
}
void trade(Runtime& r,long long id,long long bid,long long ask,double price,long long qty,TradeKind k) {
    TradeEvent t;t.app_sequence=id;t.exchange_time_us=t.local_time_us=34200000000LL+id*110000000;
    t.buy_order_id=bid;t.sell_order_id=ask;t.price=price;t.volume=qty;t.kind=k;
    SampleBuffer b;r.on_trade(t,&b);
}
int main() {
    for (bool sell : {true,false}) {
      for (bool baseline : {false,true}) {
        auto config=inputs();config.v06_baseline=baseline;Runtime r(config);
        order(r,1,true,10,100);order(r,2,true,9.99,100);
        order(r,3,false,10.02,100);order(r,4,false,10.03,100);
        order(r,5,!sell,0,500,OrderKind::kMarket);
        trade(r,6,sell?1:5,sell?5:3,sell?10:10.02,100,TradeKind::kFill);
        trade(r,7,sell?2:5,sell?5:4,sell?9.99:10.03,100,TradeKind::kFill);
        trade(r,8,sell?0:5,sell?5:0,0,300,TradeKind::kCancel);
        assert(r.available());
        OrderEvent published;bool linked=false;
        assert(r.take_resolved_market_order(&published,&linked));
        assert(linked && published.volume==500); // Published quantity remains original.
        order(r,9,sell,sell?9.98:10.04,1000);
        order(r,10,!sell,sell?9.99:10.03,1000);
        SampleBuffer out;r.flush(&out);
        if (!r.available() || out.count!=1)
            std::cerr<<"sell="<<sell<<" available="<<r.available()<<" count="<<out.count<<" reason="<<r.failure_reason()<<"\n";
        assert(r.available() && out.count==1);
        assert(out.values[0].bid_price[0]<out.values[0].ask_price[0]);
        assert(out.values[0].volume==200);
        assert(out.values[0].bid_price[0]==(sell?9.98:10.03));
      }
    }
    {
        Runtime r(inputs());order(r,1,true,10,100);order(r,2,false,10.02,100);
        order(r,3,false,0,500,OrderKind::kMarket);
        trade(r,4,1,3,10,100,TradeKind::kFill);
        trade(r,5,0,3,0,300,TradeKind::kCancel); // 100+300 != 500.
        assert(!r.available());
    }
    {
        Runtime r(inputs());order(r,1,true,10,100);order(r,2,false,10.02,100);
        order(r,3,false,0,500,OrderKind::kMarket);
        trade(r,4,0,3,0,500,TradeKind::kCancel); // No fills, full cancellation.
        assert(r.available());
    }
    {
        Runtime r(inputs());order(r,1,true,10.64,7100);order(r,2,false,10.61,1000);
        order(r,3,false,10.62,1000);
        assert(!r.available());
        const auto n=r.sample_count();order(r,4,true,10,1000);
        assert(!r.available() && r.sample_count()==n);
    }
    {
        // A crossing incoming order followed by linked fills is valid.
        Runtime r(inputs());order(r,1,true,10,1000);order(r,2,false,10.02,100);
        order(r,3,true,0,100,OrderKind::kMarket);
        trade(r,4,3,2,10.02,100,TradeKind::kFill);
        order(r,5,false,10.03,100);SampleBuffer out;r.flush(&out);
        assert(r.available());
    }
    std::cout<<"partial-fill cancel both sides, completed-cross rejection and linked-fill transient passed\n";
}
