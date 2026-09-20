#include "adapters/td/atp/AtpOmsRequests.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include <time.h>
using namespace atp::quant_api;
struct Account {
    std::string cust_id="synthetic", fund_account_id="123456789012", branch_id="0000";
    std::string account_id="A123456789", password="synthetic-only";
    int market_id=101, business_type=1;
};
void require(bool b, const char* m) { if(!b) throw std::runtime_error(m); }
template<class M> std::string bytes(const M* m) {
    const auto b=m->Encode(); return std::string(b.data,b.data_size);
}
template<class M> void same_account(const M* x,const M* y) {
    require(std::string(x->GetCustId())==y->GetCustId(),"customer mismatch");
    require(std::string(x->GetFundAccountId())==y->GetFundAccountId(),"fund mismatch");
    require(std::string(x->GetBranchId())==y->GetBranchId(),"branch mismatch");
    require(std::string(x->GetAccountId())==y->GetAccountId(),"account mismatch");
    require(std::string(x->GetPassword())==y->GetPassword(),"password mismatch");
    require(x->GetMarketId()==y->GetMarketId(),"market mismatch");
    require(x->GetBatchClOrdNo()==y->GetBatchClOrdNo(),"batch ID mismatch");
}
void same_order(const ATPReqCashAuctionOrderMsg* x,const ATPReqCashAuctionOrderMsg* y) {
    same_account(x,y);
    require(std::string(x->GetSecurityId())==y->GetSecurityId(),"stock mismatch");
    require(x->GetSide()==y->GetSide(),"side mismatch");
    require(x->GetOrderQty()==y->GetOrderQty(),"quantity mismatch");
    require(x->GetPrice()==y->GetPrice(),"price mismatch");
    require(x->GetOrderType()==y->GetOrderType(),"order type mismatch");
    require(x->GetOpuId()==y->GetOpuId(),"route channel mismatch");
}
template<class M> void fixed(M* m,const Account& a) {
    m->SetCustId(a.cust_id.c_str());m->SetFundAccountId(a.fund_account_id.c_str());
    m->SetBranchId(a.branch_id.c_str());m->SetAccountId(a.account_id.c_str());
    m->SetPassword(a.password.c_str());m->SetMarketId(a.market_id);
}
ATPReqCashAuctionOrderMsg* fresh(const Account& a,const oms::Command& c) {
    auto* m=ATPReqCashAuctionOrderMsg::NewMessage(a.business_type);fixed(m,a);
    m->SetSecurityId(c.intent.instrument.code.c_str());
    m->SetSide(c.intent.side==oms::Side::Buy?ATPSideConst::kBuy:ATPSideConst::kSell);
    m->SetOrderQty(c.intent.quantity);m->SetPrice(c.intent.price/double(oms::kMoneyScale));
    m->SetOrderType(ATPOrderTypeConst::kFixedNew);m->SetBatchClOrdNo(c.id);return m;
}
uint64_t now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
void print(const char* name,std::vector<uint64_t> a){std::sort(a.begin(),a.end());std::cout<<name<<" n="<<a.size()<<" p50_us="<<a[a.size()/2]/1000.<<" p99_us="<<a[a.size()*99/100]/1000.<<" max_us="<<a.back()/1000.<<'\n';}
int main(){try{
    Account a; atp_oms::Requests requests(a); oms::Command c;
    // Decode the encoded bytes with the SDK and compare every exposed field.
    // Raw Encode buffers also contain header bytes not initialized by NewMessage;
    // allocator perturbation changes those bytes in fresh messages as well.
    for(int i=0;i<2000;++i){
        c.id=100001+i;c.intent.instrument.code=i%2?"600000":"688327";
        c.intent.side=i%3?oms::Side::Buy:oms::Side::Sell;
        c.intent.quantity=i%2?100:1200;c.intent.price=100001+i*100;
        auto* f=fresh(a,c);
        auto* reused=requests.order(c);same_order(reused,f);
        auto* decoded=ATPReqCashAuctionOrderMsg::NewMessage(a.business_type);
        const auto wire=bytes(reused);
        require(decoded->Decode(Buffer(wire.size(),wire.data())),"order decode failed");
        same_order(decoded,f);
        ATPReqCashAuctionOrderMsg::DeleteMessage(decoded);
        ATPReqCashAuctionOrderMsg::DeleteMessage(f);
        auto* cancel=ATPReqCashCancelOrderMsg::NewMessage();fixed(cancel,a);
        cancel->SetOrigClOrdNo(999999+i);cancel->SetBatchClOrdNo(c.id);
        auto* reused_cancel=requests.cancel(c.id,999999+i);same_account(reused_cancel,cancel);
        auto* decoded_cancel=ATPReqCashCancelOrderMsg::NewMessage();
        const auto cancel_wire=bytes(reused_cancel);
        require(decoded_cancel->Decode(Buffer(cancel_wire.size(),cancel_wire.data())),"cancel decode failed");
        same_account(decoded_cancel,cancel);
        require(decoded_cancel->GetOrigClOrdNo()==cancel->GetOrigClOrdNo(),"original order mismatch");
        ATPReqCashCancelOrderMsg::DeleteMessage(decoded_cancel);
        ATPReqCashCancelOrderMsg::DeleteMessage(cancel);
        require(bytes(requests.order(c))==wire,"cancel modified order request");
    }
    Account second=a;second.market_id=102;second.account_id="B000000001";
    atp_oms::Requests isolated(second);
    require(std::string(isolated.order(c)->GetAccountId())==second.account_id,"second account mismatch");
    require(std::string(requests.order(c)->GetAccountId())==a.account_id,"account leaked across context");
    std::vector<uint64_t> old_time,new_time;
    for(int i=0;i<10000;++i){c.id=200001+i;
        uint64_t t=now();auto* m=fresh(a,c);ATPReqCashAuctionOrderMsg::DeleteMessage(m);old_time.push_back(now()-t);
        t=now();requests.order(c);new_time.push_back(now()-t);
    }
    std::cout<<"atp_request_reuse_test: PASS (2000 order/cancel encodings, account isolation)\n";
    print("fresh_create_fill_delete",old_time);print("reuse_dynamic_fields",new_time);
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
