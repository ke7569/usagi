#ifndef USAGI_ATP_OMS_REQUESTS_H
#define USAGI_ATP_OMS_REQUESTS_H
#include "atp_quant_api.h"
#include "common/oms/Types.h"
#include <memory>
#include <stdexcept>

namespace atp_oms {
// One pair per immutable account/connection generation. AtpBackend's existing
// transport mutex serializes all accesses, including destruction on close.
// These are ordinary NewMessage objects, NOT the single-use NewMessageFast API.
class Requests {
    using Order = atp::quant_api::ATPReqCashAuctionOrderMsg;
    using Cancel = atp::quant_api::ATPReqCashCancelOrderMsg;
    std::unique_ptr<Order, void (*)(Order*)> order_;
    std::unique_ptr<Cancel, void (*)(Cancel*)> cancel_;
    template<class Message, class Account>
    static void account_fields(Message* m, const Account& a) {
        m->SetCustId(a.cust_id.c_str());
        m->SetFundAccountId(a.fund_account_id.c_str());
        m->SetBranchId(a.branch_id.c_str());
        m->SetAccountId(a.account_id.c_str());
        m->SetPassword(a.password.c_str());
        m->SetMarketId(a.market_id);
    }
public:
    template<class Account>
    explicit Requests(const Account& a)
        : order_(Order::NewMessage(a.business_type), &Order::DeleteMessage),
          cancel_(Cancel::NewMessage(), &Cancel::DeleteMessage) {
        if (!order_ || !cancel_) throw std::runtime_error("ATP request construction failed");
        account_fields(order_.get(), a);
        account_fields(cancel_.get(), a);
        order_->SetOrderType(atp::quant_api::ATPOrderTypeConst::kFixedNew);
        // Touch every dynamic setter before the first live signal; no API send.
        order_->SetSecurityId(""); order_->SetSide(atp::quant_api::ATPSideConst::kBuy);
        order_->SetOrderQty(0); order_->SetPrice(0); order_->SetBatchClOrdNo(0);
        cancel_->SetOrigClOrdNo(0); cancel_->SetBatchClOrdNo(0);
    }
    Order* order(const oms::Command& c) {
        order_->SetSecurityId(c.intent.instrument.code.c_str());
        order_->SetSide(c.intent.side == oms::Side::Buy ?
            atp::quant_api::ATPSideConst::kBuy : atp::quant_api::ATPSideConst::kSell);
        order_->SetOrderQty(c.intent.quantity);
        order_->SetPrice(c.intent.price / double(oms::kMoneyScale));
        order_->SetBatchClOrdNo(c.id);
        return order_.get();
    }
    Cancel* cancel(oms::OrderId id, int64_t broker_id) {
        cancel_->SetOrigClOrdNo(broker_id);
        cancel_->SetBatchClOrdNo(id);
        return cancel_.get();
    }
};
}
#endif
