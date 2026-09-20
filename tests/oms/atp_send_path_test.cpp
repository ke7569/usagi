#include "adapters/td/atp/TDEngineGXBSE.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <time.h>
#include <cstring>

// Only this offline executable interposes the SDK send entry points. It never
// connects or logs in. All adapter validation, routing and message preparation
// run in the real TD shared library; the gateway call is a controlled stub.
namespace {
thread_local const oms::Command* expected_command=nullptr;
bool inspect=true;
std::atomic<int> calls{0};
std::atomic<int> next_status{0};
void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
uint64_t now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
void stats(std::vector<uint64_t> a){auto first=a.front();std::sort(a.begin(),a.end());std::cout<<"adapter_submit_stub_sdk n="<<a.size()<<" first_us="<<first/1000.<<" p50_us="<<a[a.size()/2]/1000.<<" p99_us="<<a[a.size()*99/100]/1000.<<" max_us="<<a.back()/1000.<<'\n';}
}
namespace atp { namespace quant_api {
ATPErrorCodeType ATPQuantAPI::ReqCashAuctionOrder(const ATPReqCashAuctionOrderMsg* m,int64_t) {
    if(inspect){require(expected_command!=nullptr,"unexpected send");const auto& c=*expected_command;
        require(std::string(m->GetSecurityId())==c.intent.instrument.code,"wrong stock");
        require(m->GetOrderQty()==c.intent.quantity,"wrong quantity");
        require(m->GetPrice()==c.intent.price/double(oms::kMoneyScale),"wrong price");
        require(m->GetBatchClOrdNo()==c.id,"wrong order id");
        require(m->GetSide()==(c.intent.side==oms::Side::Buy?ATPSideConst::kBuy:ATPSideConst::kSell),"wrong side");
        require(std::string(m->GetFundAccountId())==c.scope.account.account,"wrong account");
        require(m->GetMarketId()==101 && m->GetOrderType()==ATPOrderTypeConst::kFixedNew,"wrong market/type");
    }
    ++calls;return next_status.load();
}
ATPErrorCodeType ATPQuantAPI::ReqCashCancelOrder(const ATPReqCashCancelOrderMsg* m,int64_t) {
    require(expected_command!=nullptr,"unexpected cancel");const auto& c=*expected_command;
    require(m->GetOrigClOrdNo()==std::stoll(c.broker_id),"wrong broker order");
    require(m->GetBatchClOrdNo()==c.id,"wrong cancel id");
    require(std::string(m->GetFundAccountId())==c.scope.account.account,"wrong cancel account");
    ++calls;return next_status.load();
}
}}
WC_NAMESPACE_START
struct AtpSendTest {
    static void run(){
        std::ifstream maps("/proc/self/maps");std::string mapped;
        while(std::getline(maps,mapped)) if(mapped.find("libsse_td.so")!=std::string::npos && mapped.find("r-xp")!=std::string::npos)
            std::cout<<"loaded_plugin="<<mapped.substr(mapped.find('/'))<<'\n';
        TDEngineGXBSE e;
        atp::quant_api::ATPProperties properties;atp::quant_api::ATPQuantHandler handler;
        e.account_units_.resize(1);auto& unit=e.account_units_[0];
        unit.cust_id="synthetic";unit.fund_account_id="123456789012";
        unit.branch_id="0000";unit.account_id="A123456789";unit.password="synthetic-only";
        unit.market_id=101;unit.business_type=1;
        unit.api.reset(new atp::quant_api::ATPQuantAPI(&handler,&properties));
        unit.logged_in.store(true);unit.connected.store(true);
        e.stream_mode_=true;e.stream_allow_orders_=true;
        oms::Scope scope;scope.account.broker="guoxin";scope.account.account=unit.fund_account_id;
        scope.day=20260916;scope.epoch=123;scope.source=190;scope.gateway="test";
        auto backend=std::static_pointer_cast<oms::AtpBackend>(e.make_oms_backend(0,scope));
        backend->connected();backend->advance_to(1);
        oms::Command c;c.scope=scope;c.id=1;c.intent.instrument={"SSE","688327"};
        c.intent.quantity=200;c.intent.price=100000;c.intent.side=oms::Side::Buy;
        setenv("SSE_ENABLE_LIVE_ORDER","YES",1);
        std::vector<uint64_t> timings;inspect=false;
        for(int i=0;i<10000;++i){c.id=i+1;c.intent.price=100000+i*100;
            expected_command=&c;const auto t=now();const auto r=backend->submit(c);timings.push_back(now()-t);
            require(r.disposition==oms::SendDisposition::Submitted,"submission failed");}
        inspect=true;
        const int old_calls=calls.load();
        require(backend->submit(c).error.category==oms::ErrorCategory::Duplicate,"duplicate accepted");
        c.id=12000;c.intent.quantity=0;
        require(backend->submit(c).error.category==oms::ErrorCategory::Invalid,"zero quantity accepted");
        c.intent.quantity=200;c.scope.epoch++;
        require(backend->submit(c).error.failed(),"foreign epoch accepted");c.scope=scope;
        setenv("SSE_ENABLE_LIVE_ORDER","NO",1);
        require(backend->submit(c).error.category==oms::ErrorCategory::NotReady,"permission ignored");
        setenv("SSE_ENABLE_LIVE_ORDER","YES",1);
        unit.connected.store(false);
        require(backend->submit(c).error.category==oms::ErrorCategory::NotReady,"disconnect ignored");unit.connected.store(true);
        require(calls.load()==old_calls,"rejected commands reached SDK");
        for(int i=0;i<100;++i){c.id=13000+i;c.intent.instrument.code=i%2?"600000":"688327";
            c.intent.side=i%2?oms::Side::Sell:oms::Side::Buy;c.intent.quantity=i%2?100:200;
            expected_command=&c;require(!backend->submit(c).error.failed(),"varied submission failed");
            c.cancel=true;c.broker_id=std::to_string(999999+i);
            require(!backend->cancel(c).error.failed(),"cancel failed");c.cancel=false;
        }
        c.id=50000;c.cancel=true;c.broker_id="100";
        require(backend->cancel(c).error.category==oms::ErrorCategory::Ownership,"unowned cancel accepted");
        c.id=13000;c.broker_id="bad";
        require(backend->cancel(c).error.category==oms::ErrorCategory::MissingId,"invalid broker id accepted");
        // Restored orders have no original request id; cancellation still owns
        // the stable record recovered from the broker snapshot.
        TDEngineGXBSE::OrderRoute recovered;recovered.oms_scope=scope;recovered.order_ref=50000;
        recovered.oms_instrument=c.intent.instrument;recovered.oms_backend=backend;
        e.clord_to_route_[7654321]=recovered;e.oms_routes_[std::make_pair(0,50000L)]=&e.clord_to_route_.at(7654321);
        c.id=50000;c.broker_id="7654321";expected_command=&c;
        require(!backend->cancel(c).error.failed(),"recovered cancel failed");
        c.cancel=false;c.id=60000;next_status.store(12345);
        const auto unknown=backend->submit(c);
        require(unknown.disposition==oms::SendDisposition::Unknown && unknown.error.raw_code==12345,"SDK ambiguity changed");
        next_status.store(0);
        auto worker=[&](int start){oms::Command x=c;for(int i=0;i<100;++i){x.id=start+i;x.intent.instrument.code=i%2?"600004":"600006";expected_command=&x;require(!backend->submit(x).error.failed(),"concurrent submit failed");}};
        std::thread one(worker,70000),two(worker,80000);one.join();two.join();
        // Ownership pointers must survive growth; callbacks still recover the
        // original request's fields and the correct broker id.
        const auto* route=e.oms_routes_.at(std::make_pair(0,1L));
        int64_t rid=0;for(const auto& r:e.request_to_route_)if(&r.second==route)rid=r.first;
        require(rid>0,"route pointer lost");e.bind_clord_route(rid,111111);
        TDEngineGXBSE::OrderRoute found;
        require(e.lookup_route_by_clord(111111,&found) && found.order_ref==1,"callback mapping failed");
        backend->close();expected_command=&c;require(backend->submit(c).error.failed(),"closed backend accepted");
        unit.api.reset(); // No Login/Connect/Release request to any external system.
        std::cout<<"atp_send_path_test: PASS (routing, cancel, recovery, permissions, error, concurrency)\n";stats(timings);
    }
};
WC_NAMESPACE_END
int main(){try{USING_WC_NAMESPACE AtpSendTest::run();}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
