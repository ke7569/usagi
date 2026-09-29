#include "common/execution/AtpSnapshotOrder.h"
#include "common/oms/Oms.h"
#include <cassert>
#include <iostream>

using namespace oms;
int main() {
    SnapshotOrder row; row.instrument={"SSE","688327"};row.side=Side::Buy;
    row.broker_id="100000001";row.price=96300;row.original=200;
    row.filled=0;row.working=200;row.state=OrderState::Canceled;
    const auto raw=row;
    assert(strategy_runtime::normalize_atp_snapshot_order(&row,200));
    assert(row.original==200 && row.filled==0 && row.working==0);
    Config c;c.enabled=true;c.scope.account.broker="paper";c.scope.account.account="snapshot-test";
    c.scope.gateway="test";c.scope.day=20260914;c.scope.source=190;c.instance="snapshot-test";
    InstrumentRules rules;rules.lower_price=100;rules.upper_price=200000;rules.tick=100;rules.lot=100;
    c.instruments[row.instrument]=rules;
    Capabilities caps;caps.simulated=true;caps.complete_snapshot=true;
    caps.fills=FillCoverage::DualFromOrigin;caps.trades_required_for_snapshot=true;
    std::shared_ptr<ScriptedBackend> backend(new ScriptedBackend(caps));
    auto engine=Engine::create(c,backend);assert(engine->start_epoch(1));assert(engine->begin_reconcile(1,false));
    Snapshot s;s.scope=engine->scope();s.token=1;s.free_cash=1000000000;
    s.account_success=s.positions_success=s.orders_success=s.trades_success=true;
    s.all_day_orders=s.all_day_trades=true;
    SnapshotPosition p;p.instrument=row.instrument;p.total=p.free_sellable=0;s.positions.push_back(p);
    s.orders.push_back(raw);backend->publish(s);
    assert(!engine->account().ready); // Reproduce today's actual SDK row rejection.
    assert(engine->begin_reconcile(2,false));s.token=2;s.orders[0]=row;backend->publish(s);
    assert(engine->account().ready && engine->account().cash_reserved==0);
    row=raw;row.state=OrderState::Accepted;
    assert(strategy_runtime::normalize_atp_snapshot_order(&row,0) && row.working==200);
    row.filled=50;row.working=150;row.state=OrderState::Partial;
    assert(strategy_runtime::normalize_atp_snapshot_order(&row,0) && row.working==150);
    row.state=OrderState::Canceled;
    assert(strategy_runtime::normalize_atp_snapshot_order(&row,150) && row.filled==50 && !row.working);
    row=raw;row.working=201;assert(!strategy_runtime::normalize_atp_snapshot_order(&row,200));
    row=raw;row.state=OrderState::Filled;assert(!strategy_runtime::normalize_atp_snapshot_order(&row,0));
    row=raw;row.filled=200;row.working=0;row.state=OrderState::Filled;
    assert(strategy_runtime::normalize_atp_snapshot_order(&row,0));
    std::cout<<"atp_snapshot_order_test: PASS\n";
}
