#include "adapters/td/atp/AtpOrderReportMapping.h"
#include "common/oms/Oms.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace oms;
const Instrument instrument = {"SSE", "600151"};
const std::int64_t original = 100000001;
const std::int64_t cancellation = 100000002;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}

struct Fixture {
 std::shared_ptr<ScriptedBackend> backend;std::shared_ptr<Engine> engine;OrderId id;
 Fixture(){
  Config c;c.scope.account.broker="fixture";c.scope.account.account="fixture";c.scope.gateway="atp-fixture";
  c.scope.day=20260914;c.scope.source=190;c.instance="cancel-regression";c.enabled=true;c.ownership=OwnershipMode::Simulation;
  InstrumentRules rules;rules.tick=100;rules.lot=100;rules.lower_price=90000;rules.upper_price=110000;c.instruments[instrument]=rules;
  Capabilities caps;caps.simulated=true;caps.complete_snapshot=true;caps.fills=FillCoverage::DualFromOrigin;caps.trades_required_for_snapshot=true;
  backend.reset(new ScriptedBackend(caps));
  backend->submit_hook=[](const Command&){SendResult r;r.disposition=SendDisposition::Submitted;return r;};
  backend->cancel_hook=[](const Command&){SendResult r;r.disposition=SendDisposition::Submitted;return r;};
  engine=Engine::create(c,backend);require(engine->start_epoch(1),"start epoch");require(engine->begin_reconcile(1,false),"begin reconcile");
  Snapshot snap;snap.scope=engine->scope();snap.token=1;snap.free_cash=1000000000000LL;
  snap.account_success=snap.positions_success=snap.orders_success=snap.trades_success=true;snap.all_day_orders=snap.all_day_trades=true;
  SnapshotPosition pos;pos.instrument=instrument;snap.positions.push_back(pos);require(engine->complete_snapshot(snap),"complete snapshot");
  Intent intent;intent.owner="fixture";intent.intent_id="order-1";intent.signal_id="signal-1";intent.instrument=instrument;
  intent.side=Side::Buy;intent.type=OrderType::Limit;intent.price=97400;intent.quantity=200;
  const SubmitResult submitted=engine->submit(intent);require(submitted.accepted,"submit accepted");id=submitted.id;
 }
 Report report(std::int64_t broker,OrderState state,Quantity cumulative=0,Quantity leaves=200)const{
  Report r;r.scope=engine->scope();r.id=id;r.broker_id=std::to_string(broker);r.instrument=instrument;r.side=Side::Buy;
  r.kind=ReportKind::Order;r.state=state;r.original=200;r.cumulative=cumulative;r.leaves=leaves;return r;
 }
 // The same production mapping decides whether an ATP return may reach OMS.
 // Unknown original IDs follow the adapter's existing disconnect/reconcile path.
 void returned(std::int64_t cl,std::int64_t orig,std::uint8_t sign,OrderState state,Quantity cumulative=0,Quantity leaves=200){
  const atp_order_report::Mapping mapping=atp_order_report::map(cl,orig,sign);
  if(mapping.cancel_object){
   if(mapping.route_cl_ord_no!=original)engine->set_connected(engine->scope(),false);
   if(!mapping.bind_report_cl_ord && !mapping.publish_oms_order)return;
  }
  engine->report(report(cl,state,cumulative,leaves));
 }
 OrderView view()const{OrderView v;require(engine->order(id,&v),"order view");return v;}
 void ready_original()const{if(!engine->account().ready)throw std::runtime_error("account must remain ready: "+engine->account().reason);require(view().command.broker_id==std::to_string(original),"original broker identity changed");}
 void accept(){returned(original,0,0,OrderState::Accepted);ready_original();}
 void cancel_result(bool rejected){
  engine->cancel("fixture",id);Report r=report(original,OrderState::Accepted);r.broker_id.clear();r.kind=ReportKind::CancelResult;
  if(rejected){r.error.category=ErrorCategory::Rejected;r.error.raw_code=1;r.error.raw_type="fixture";}
  engine->report(r);
 }
};

void test_historical_failure_is_detected_by_oms(){
 Fixture f;f.accept();f.cancel_result(false);
 f.engine->report(f.report(cancellation,OrderState::Accepted));
 require(!f.engine->account().ready,"historical wrong broker identity must still freeze OMS");
}
void test_cancel_object_cannot_finalize_or_rebind_original(){
 Fixture f;f.accept();f.cancel_result(false);
 f.returned(cancellation,original,1,OrderState::Accepted);
 f.returned(cancellation,original,1,OrderState::Accepted);
 f.returned(cancellation,original,1,OrderState::Canceled,0,0);
 f.ready_original();require(f.view().state!=OrderState::Canceled,"cancel object prematurely canceled original");
 require(f.engine->has_working_order(instrument),"original must remain working until its own terminal return");
 f.returned(original,0,0,OrderState::Canceled,0,0);
 f.ready_original();require(f.view().state==OrderState::Canceled,"original canceled return not applied");
 require(!f.engine->has_working_order(instrument),"canceled original still working");
 f.returned(cancellation,original,1,OrderState::Canceled,0,0);f.ready_original();
}
void test_cancel_rejection_retains_working_original(){
 Fixture f;f.accept();f.cancel_result(true);
 const std::string guarded_reason=f.engine->account().reason;
 require(!f.engine->account().ready && guarded_reason=="cancel rejected without terminal order proof","real cancel rejection must retain existing OMS guard");
 f.returned(cancellation,original,1,OrderState::Rejected);
 require(!f.engine->account().ready && f.engine->account().reason==guarded_reason,"cancel object changed rejection handling");
 require(f.view().command.broker_id==std::to_string(original),"cancel rejection changed broker identity");
 require(f.engine->has_working_order(instrument),"cancel rejection lost original working order");
 require(f.view().state!=OrderState::Rejected,"cancel rejection rejected the original order");
}
void test_normal_partial_fill_survives_cancel_object(){
 Fixture f;f.accept();f.returned(original,0,0,OrderState::Partial,100,100);
 Report trade=f.report(original,OrderState::Partial,100,100);trade.kind=ReportKind::Trade;
 trade.trade_id="fill-1";trade.trade_quantity=100;trade.cumulative_after=100;trade.trade_price=97400;trade.trade_fee=0;
 f.engine->report(trade);f.ready_original();
 f.returned(cancellation,original,1,OrderState::Accepted,0,200);f.ready_original();
 require(f.view().state==OrderState::Partial,"cancel object replaced partial fill state");
}
void test_unowned_cancel_remains_not_ready(){
 Fixture f;f.accept();f.returned(cancellation,999999999,1,OrderState::Accepted);
 require(!f.engine->account().ready,"unknown original was silently treated as owned");
}
}
int main(){
 const char* test="historical_failure";
 try{test_historical_failure_is_detected_by_oms();test="cancel_lifecycle";test_cancel_object_cannot_finalize_or_rebind_original();
 test="cancel_rejection";test_cancel_rejection_retains_working_original();test="partial_fill";test_normal_partial_fill_survives_cancel_object();
 test="unowned_cancel";test_unowned_cancel_remains_not_ready();}
 catch(const std::exception& e){std::cerr<<"atp_cancel_lifecycle_test ("<<test<<"): "<<e.what()<<'\n';return 1;}
 std::cout<<"atp_cancel_lifecycle_test: PASS\n";return 0;
}
