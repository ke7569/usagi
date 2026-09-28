// Standalone fake-transport harness executes the real ZStrategyV06.cpp bodies.
// It deliberately has no SDK connection and cannot submit a real order.
#define ZSTRATEGY_H
#define STATEGY_BASE_H
#include "V06AccountState.h"
#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>
#define KF_LOG_INFO(logger, expression) do {} while (0)
enum { LF_CHAR_Buy='0', LF_CHAR_Sell='1', LF_CHAR_Open='0', LF_CHAR_Close='1',
       LF_CHAR_AllTraded='0', LF_CHAR_PartTradedQueueing='1', LF_CHAR_PartTradedNotQueueing='2',
       LF_CHAR_NoTradeNotQueueing='4', LF_CHAR_Canceled='5', LF_CHAR_Error='a' };
enum DirectionEnum { BUY, SELL };
enum OrderTypeEnum { FAK, LMP };
struct RT_Order { RT_Order(double,int,DirectionEnum,OrderTypeEnum) {} };
struct MSMarketDataField { double BidPrice1=10,AskPrice1=10.01,LastPrice=10,BidVolume1=200,AskVolume1=200; };
struct LFRtnOrderField { int VolumeTraded=0,VolumeTotalOriginal=0,VolumeTotal=0; char OrderStatus='1'; };
struct LFRtnTradeField { int VolumeTraded=0,VolumeTotal=0,Volume=0; double Price=0; char TradingDay[9]={},OrderSysID[31]={},TradeID[31]={}; };
typedef std::function<void()> BLCallback;
struct FakeUtil {
    long long now=1000000000LL; int next=1,cancels=0,submits=0,last_qty=0;
    std::function<void(int)> inline_order_callback;
    std::vector<std::pair<long long,BLCallback> > callbacks;
    int insert_limit_order(short,const std::string&,const std::string&,double,int qty,char,char) {
        ++submits; last_qty=qty; const int id=next++;
        if(inline_order_callback)inline_order_callback(id); return id;
    }
    int cancel_order(short,int id) { ++cancels; return id; }
    void insert_callback(long long when,BLCallback& callback){callbacks.push_back({when,callback});}
    long long get_nano()const{return now;}
};
class ZStrategy {
public:
    FakeUtil fake; FakeUtil* util=&fake;
    std::string mTradeInstrument="000001",ExchangeID="SZE";
    short td_source_=180; bool v06_enabled_=true,v06_submitting_=false,routing_enabled_=true,virtual_routing_=false;
    int startup_signal_count_=0,max_position_=200;
    double v06_upper_price_=0,v06_lower_price_=0;
    struct Params { int static_position=1000,last_position=0,shortable=1000; double max_order_size=1e9; } i_params;
    struct Context { int pi=0,vl_pos=0,vs_pos=0,cum_buy=0,cum_sell=0; } context;
    std::shared_ptr<V06AccountState> v06_account_;
    v06_strategy::Config v06_config_;
    v06_strategy::ReservationBook v06_reservations_;
    struct V06Order { int original=0,cumulative=0; bool buy=true,terminal=false,cancel_pending=false; double price=0;
        long long cancel_retry_ns=0; v06_strategy::OrderKind kind=v06_strategy::OrderKind::Hit; };
    std::map<int,V06Order> v06_orders_;
    std::vector<std::pair<LFRtnOrderField,int> > v06_early_orders_;
    std::vector<std::pair<LFRtnTradeField,int> > v06_early_trades_;
    void bind_v06_account(const std::shared_ptr<V06AccountState>&);
    void halt_v06_for_day(const std::string&);
    void on_v06_signal(const MSMarketDataField*,const std::array<float,4>&,std::uint64_t,short,long);
    void v06_order_return(const LFRtnOrderField*,int);
    void v06_trade_return(const LFRtnTradeField*,int);
    void v06_cancel(int); int v06_submit(const v06_strategy::PricingOutput&,int);
    bool can_send_order(DirectionEnum,long long)const{return true;}
    void on_order_reject(int,const RT_Order&){}
};
#include "ZStrategyV06.cpp"

static void initialize(ZStrategy& s) {
    s.bind_v06_account(std::make_shared<V06AccountState>());
    s.v06_account_->sync_position("000001",1000);
    s.v06_account_->observe_open("000001",10);
}
static v06_strategy::PricingOutput decision() {
    v06_strategy::PricingOutput d; d.side=v06_strategy::Side::Buy;
    d.kind=v06_strategy::OrderKind::Hit;d.order_price=10; return d;
}
static LFRtnTradeField trade() {
    LFRtnTradeField t;t.Volume=100;t.VolumeTraded=100;t.VolumeTotal=100;t.Price=10;
    std::strcpy(t.TradingDay,"20260910");std::strcpy(t.OrderSysID,"11");std::strcpy(t.TradeID,"t1");return t;
}
static void initialize_twenty_stock_account(V06AccountState& account, int first_total=200) {
    for (int i=0;i<20;++i) {
        const std::string code=std::to_string(100001+i);
        account.register_instrument(code,200);
        account.sync_position(code,i==0 ? first_total : 200);
        assert(account.observe_open(code,15));
    }
    assert(account.ready());
}
static void assert_skew(const v06_strategy::DirectionalSkew& skew, double buy, double sell) {
    assert(skew.valid);
    assert(std::abs(skew.buy_bps-buy)<1e-10);
    assert(std::abs(skew.sell_bps-sell)<1e-10);
}
int main() {
    {
        char temp[]="/tmp/v06-daily-block-XXXXXX";
        char* directory=mkdtemp(temp);assert(directory);
        ZStrategy s;s.bind_v06_account(std::make_shared<V06AccountState>());
        s.v06_account_->configure_book_guard(directory);
        s.v06_account_->sync_position("000001",1000);
        s.v06_account_->observe_open("000001",10);
        s.v06_submit(decision(),100);
        MSMarketDataField md;md.BidPrice1=10.64;md.AskPrice1=10.22;
        std::array<float,4> h={{-10,-10,-10,-10}};
        const auto now=(10*3600LL)*1000000LL;
        s.on_v06_signal(&md,h,now,180,0);
        assert(s.v06_account_->blocked("000001") && s.fake.cancels==1 && s.fake.submits==1);
        md.BidPrice1=10;md.AskPrice1=10.01;
        s.on_v06_signal(&md,h,now,180,0);
        assert(s.fake.submits==1 && s.v06_submit(decision(),100)==-1);
        V06AccountState restarted;restarted.configure_book_guard(directory);
        restarted.register_instrument("000001",1000);
        assert(restarted.blocked("000001") && !restarted.needs_market_data("000001"));
        restarted.register_instrument("000002",0);restarted.sync_position("000002",100);
        restarted.observe_open("000002",10);assert(restarted.ready());
        assert(!restarted.blocked("000002"));
        unlink((std::string(directory)+"/000001.blocked").c_str());rmdir(directory);
    }
    {
        V06AccountState a;
        a.register_instrument("000001",0);
        assert(a.needs_market_data("000001"));
        assert(!a.ready());
        a.sync_position("000001",0);
        assert(a.ready() && !a.needs_market_data("000001"));
        v06_strategy::Config c;
        assert_skew(a.skew(100000000LL,c),0,0);
    }
    {
        v06_strategy::PositionClampInput p;
        v06_strategy::Config c;
        p.static_position=0;p.opening_position=1000;p.current_position=1000;
        p.side=v06_strategy::Side::Sell;p.requested=1000;
        p.exchange_time_micros=(9*3600LL+31*60LL)*1000000LL;
        assert(v06_strategy::ordinaryAllowedVolume(p,c)==1000);
        p.dirty_sell_hit=200;p.dirty_sell_quote=300;
        assert(v06_strategy::ordinaryAllowedVolume(p,c)==500);
        p.short_position=400;p.current_position=600;
        assert(v06_strategy::ordinaryAllowedVolume(p,c)==100);
        p.side=v06_strategy::Side::Buy;
        assert(v06_strategy::ordinaryAllowedVolume(p,c)==0);
        p.side=v06_strategy::Side::Sell;p.current_position=0;
        assert(v06_strategy::ordinaryAllowedVolume(p,c)==0);
    }
    {
        // Real signal/submit/reply path: sell-only, available inventory,
        // partial fill, duplicate replies, pending cancellation and no rebuy.
        ZStrategy s;s.i_params.static_position=0;s.i_params.last_position=1000;
        s.i_params.shortable=300;s.context.pi=1000;
        s.bind_v06_account(std::make_shared<V06AccountState>());
        MSMarketDataField md;
        std::array<float,4> sell={{-10,-10,-10,-10}},buy={{10,10,10,10}};
        const auto now=(10*3600LL)*1000000LL;
        s.on_v06_signal(&md,sell,now,180,0);assert(s.fake.submits==0);
        s.v06_account_->sync_position("000001",1000);
        s.v06_account_->observe_open("000001",10);
        assert(s.v06_account_->ready());
        assert_skew(s.v06_account_->skew(now,s.v06_config_),0,0);
        s.on_v06_signal(&md,buy,now,180,0);assert(s.fake.submits==0);
        assert(s.v06_submit(decision(),100)==-1 && s.fake.submits==0);
        s.on_v06_signal(&md,sell,now,180,0);
        assert(s.fake.submits==1 && s.fake.last_qty==200 && !s.v06_orders_[1].buy);
        s.on_v06_signal(&md,sell,now,180,0);
        assert(s.fake.submits==2 && s.fake.last_qty==100 && s.context.vs_pos==300);
        s.on_v06_signal(&md,sell,now,180,0);assert(s.fake.submits==2);
        s.v06_cancel(1);
        s.on_v06_signal(&md,sell,now,180,0);assert(s.fake.submits==2);
        LFRtnOrderField o;o.VolumeTraded=100;o.VolumeTotal=100;o.VolumeTotalOriginal=200;
        s.v06_order_return(&o,1);
        assert(s.context.pi==900 && s.i_params.shortable==200 && s.context.vs_pos==200);
        s.on_v06_signal(&md,sell,now,180,0);assert(s.fake.submits==2);
        auto t=trade();s.v06_trade_return(&t,1);s.v06_trade_return(&t,1);
        s.v06_order_return(&o,1);
        assert(s.context.pi==900 && s.i_params.shortable==200 && s.context.cum_sell==100);
        o.OrderStatus=LF_CHAR_Canceled;o.VolumeTotal=0;s.v06_order_return(&o,1);
        s.on_v06_signal(&md,sell,now,180,0);
        assert(s.fake.submits==3 && s.fake.last_qty==100 && s.context.vs_pos==200);
    }
    {
        ZStrategy s;s.i_params.static_position=0;s.i_params.last_position=100;
        s.i_params.shortable=100;s.context.pi=100;
        s.bind_v06_account(std::make_shared<V06AccountState>());
        s.v06_account_->sync_position("000001",100);
        s.v06_account_->observe_open("000001",10);
        MSMarketDataField md;std::array<float,4> h={{-10,10,10,10}};
        const auto now=(10*3600LL)*1000000LL;
        s.on_v06_signal(&md,h,now,180,0);
        assert(s.fake.submits==1 && s.fake.last_qty==100); // MH4 cannot block reduction.
        auto t=trade();s.v06_trade_return(&t,1);
        assert(s.context.pi==0 && s.i_params.shortable==0);
        s.on_v06_signal(&md,h,now,180,0);
        h={{10,10,10,10}};s.on_v06_signal(&md,h,now,180,0);
        assert(s.fake.submits==1);
    }
    {
        // Exercise the real account-to-rules boundary with the live basket
        // size. Twenty positions and zero fills must produce a valid skew.
        V06AccountState account;initialize_twenty_stock_account(account);
        v06_strategy::Config config;
        assert_skew(account.skew(100000000LL,config),0,0);

        // Base value is 20 * 200 * 15 = 60000. A 1500 buy gives
        // exposure .025 and a 2.5 bps buy penalty at the next snapshot.
        account.observe_cumulative("buy-order",100);
        assert(!account.ready());
        account.fill("buy-fill",true,100,15,"buy-order");
        assert(account.ready());
        assert_skew(account.skew(109999999LL,config),0,0);
        assert_skew(account.skew(110000000LL,config),2.5,0);
        account.fill("buy-fill",true,100,15,"buy-order");
        assert(account.ready());
        assert_skew(account.skew(120000000LL,config),2.5,0);

        // A later sell changes the account-wide net, while every strategy
        // instance continues to see the same frozen ten-second snapshot.
        account.observe_cumulative("sell-order",300);
        account.fill("sell-fill",false,300,15,"sell-order");
        assert(account.ready());
        assert_skew(account.skew(129999999LL,config),2.5,0);
        assert_skew(account.skew(130000000LL,config),0,10);
        account.observe_cumulative("flatten-order",200);
        account.fill("flatten-fill",true,200,15,"flatten-order");
        assert(account.ready());
        assert_skew(account.skew(139999999LL,config),0,10);
        assert_skew(account.skew(140000000LL,config),0,0);
    }
    {
        // Carried holdings above/below the static basket enter initial_net
        // even when there are no executions today.
        V06AccountState long_account;initialize_twenty_stock_account(long_account,300);
        V06AccountState short_account;initialize_twenty_stock_account(short_account,100);
        v06_strategy::Config config;
        assert_skew(long_account.skew(100000000LL,config),2.5,0);
        assert_skew(short_account.skew(100000000LL,config),0,2.5);
    }
    {
        V06AccountState a;a.register_instrument("000001",1000);a.sync_position("000001",1000);
        const unsigned auction=(9*3600+25*60)*1000, continuous=(9*3600+30*60)*1000;
        assert(!a.observe_execution_open("000001",10,auction,20260909,20260910));
        assert(!a.observe_execution_open("000001",10,auction-1,20260910,20260910));
        assert(!a.ready());
        assert(a.observe_execution_open("000001",11,continuous+1234,20260910,20260910));
        assert(a.ready());
        assert(!a.observe_execution_open("000001",12,continuous+2234,20260910,20260910));
        V06AccountState b;b.register_instrument("000002",1000);b.sync_position("000002",1000);
        assert(b.observe_execution_open("000002",20,auction,20260910,20260910));
        assert(!b.observe_execution_open("000002",21,continuous,20260910,20260910));
        V06AccountState c;c.register_instrument("000003",1000);c.sync_position("000003",1000);
        assert(!c.observe_execution_open("000003",0,continuous,20260910,20260910));
        assert(!c.observe_execution_open("000003",1,15*3600*1000+1,20260910,20260910));
        assert(c.observe_execution_open("000003",1,15*3600*1000,20260910,20260910));
    }
    {
        V06AccountState a; a.register_instrument("000001",1000);
        a.observe_open("000001",10); assert(!a.ready()); // Unknown query is never zero holdings.
        a.sync_position("000001",1000);assert(a.ready());
        a.register_instrument("000002",500);a.sync_position("000002",500);assert(!a.ready());
        a.observe_open("000002",20);assert(a.ready());
        a.observe_cumulative("o1",100);assert(!a.ready());
        a.fill("t1",true,100,10,"o1");assert(a.ready());
        a.fill("t1",true,100,10,"o1");assert(a.ready());
        a.fill("t1",true,100,11,"o1");assert(!a.ready());
    }
    {
        ZStrategy s;initialize(s);int id=s.v06_submit(decision(),200);
        LFRtnOrderField o;o.VolumeTraded=100;o.VolumeTotal=100;o.VolumeTotalOriginal=200;
        s.v06_order_return(&o,id);assert(s.context.pi==100&&s.context.vl_pos==100);assert(!s.v06_account_->ready());
        auto t=trade();s.v06_trade_return(&t,id);s.v06_trade_return(&t,id);s.v06_order_return(&o,id);
        assert(s.context.pi==100&&s.context.cum_buy==100&&s.v06_account_->ready());
        s.v06_cancel(id);s.v06_cancel(id);assert(s.fake.cancels==1&&s.context.vl_pos==100);
        s.v06_order_return(&o,id);s.v06_cancel(id);assert(s.fake.cancels==1); // Partial ack preserves cancel pending.
        o.OrderStatus=LF_CHAR_Canceled;o.VolumeTotal=0;s.v06_order_return(&o,id);s.v06_order_return(&o,id);
        assert(s.context.vl_pos==0&&s.context.pi==100);s.v06_cancel(id);assert(s.fake.cancels==1);
    }
    {
        ZStrategy s;initialize(s);int id=s.v06_submit(decision(),200);auto t=trade();
        s.v06_trade_return(&t,id);assert(s.context.pi==100&&s.context.vl_pos==100&&s.v06_account_->ready());
        LFRtnOrderField o;o.VolumeTraded=100;o.VolumeTotal=100;s.v06_order_return(&o,id);
        assert(s.context.pi==100);
    }
    {
        ZStrategy s;initialize(s);
        s.fake.inline_order_callback=[&](int id){LFRtnOrderField o;o.VolumeTotal=200;s.v06_order_return(&o,id);};
        const int id=s.v06_submit(decision(),200);assert(s.v06_account_->ready()&&s.context.vl_pos==200);
        s.v06_cancel(id);s.fake.now+=6000000000LL;s.v06_cancel(id);assert(s.fake.cancels==2&&s.context.vl_pos==200);
    }
    {
        ZStrategy s;initialize(s);s.context.pi=100;
        MSMarketDataField md;std::array<float,4> h={{-10,std::numeric_limits<float>::quiet_NaN(),-10,-10}};
        s.on_v06_signal(&md,h,(9*3600LL+34*60LL)*1000000LL,180,0);
        assert(s.fake.submits==1&&s.fake.last_qty==100); // Invalid aux permits reduction without 50-sample warmup.
    }
    {
        ZStrategy s;initialize(s);s.v06_submit(decision(),200);
        MSMarketDataField md;std::array<float,4> h={{std::numeric_limits<float>::quiet_NaN(),1,1,1}};
        s.on_v06_signal(&md,h,(9*3600LL+34*60LL)*1000000LL,180,0);
        assert(s.fake.submits==1&&s.fake.cancels==1&&s.context.vl_pos==200);
    }
    {
        V06AccountState a;a.register_instrument("000001",1000);a.sync_position("000001",1000);a.observe_open("000001",10);
        v06_strategy::Config c;auto before=a.skew(100000000LL,c);assert(before.valid&&before.buy_bps==0);
        a.observe_cumulative("o1",100);a.fill("t1",true,100,10,"o1");
        auto same=a.skew(109000000LL,c);assert(same.buy_bps==0);
        auto next=a.skew(110000000LL,c);assert(next.buy_bps==10&&next.sell_bps==0);
    }
    {
        ZStrategy s;initialize(s);auto d=decision();d.kind=v06_strategy::OrderKind::Quote;
        const int id=s.v06_submit(d,100);assert(s.fake.callbacks.empty());
        MSMarketDataField md;std::array<float,4> h={{1,1,1,1}};
        s.on_v06_signal(&md,h,(14*3600LL+56*60LL)*1000000LL,180,0);
        assert(s.fake.cancels==1&&s.context.vl_pos==100);
        LFRtnOrderField o;o.OrderStatus=LF_CHAR_Canceled;s.v06_order_return(&o,id);assert(s.context.vl_pos==0);
    }
    {
        ZStrategy s;initialize(s);s.v06_upper_price_=10;
        MSMarketDataField md;std::array<float,4> h={{10,10,10,10}};
        s.on_v06_signal(&md,h,(9*3600LL+34*60LL)*1000000LL,180,0);
        assert(s.fake.submits==0); // Best ask above an authoritative exchange limit.
    }
    std::cout<<"V06 live execution: 20-stock exposure and snapshots, account gates, open, trade/order reorder+duplicates, partial cancel, inline callback, retries, MH4 reduction passed\n";
}
