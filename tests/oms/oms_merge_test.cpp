// OMS merge item tests (OMS-1):
//  - Engine report idempotency gaps: identical duplicate Accepted/Filled,
//    late terminal repeats, cancel-result after terminal.
//  - Engine has_working_order() primitive used by the SSE single-flight gate.
//  - AtpBackend closed-loop contract: bound sinks receive an echoed report
//    exactly once and receive disconnect notifications.
#include "common/oms/Oms.h"
#include "common/oms/Backend.h"
#include "adapters/td/atp/OmsAtpBackend.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace oms;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

const Instrument kSze = {"SZE", "000001"};
const Instrument kSse = {"SSE", "600000"};

InstrumentRules rules() {
    InstrumentRules result;
    result.tick = 100;
    result.lot = 100;
    result.lower_price = 90000;
    result.upper_price = 110000;
    return result;
}

struct Fixture {
    Config config;
    Capabilities capabilities;
    std::shared_ptr<ScriptedBackend> backend;
    std::shared_ptr<Engine> engine;

    Fixture(bool restart_cancel = false) {
        config.scope.account.broker = "paper";
        config.scope.account.account = "unit";
        config.scope.gateway = "fixture";
        config.scope.day = 20260904;
        config.scope.source = 1;
        config.instance = "test";
        config.enabled = true;
        config.ownership = OwnershipMode::Simulation;
        config.restart_cancel_open_orders = restart_cancel;
        config.instruments[kSze] = rules();
        config.instruments[kSse] = rules();
        capabilities.simulated = true;
        capabilities.complete_snapshot = true;
        capabilities.fills = FillCoverage::DualFromOrigin;
        capabilities.trades_required_for_snapshot = true;
        backend.reset(new ScriptedBackend(capabilities));
        engine = Engine::create(config, backend);
        require(engine->start_epoch(1), "start epoch");
        require(engine->begin_reconcile(1, false), "begin reconcile");
        Snapshot snapshot;
        snapshot.scope = engine->scope();
        snapshot.token = 1;
        snapshot.free_cash = 1000000000000LL;
        snapshot.account_success = true;
        snapshot.positions_success = true;
        snapshot.orders_success = true;
        snapshot.trades_success = true;
        snapshot.all_day_orders = true;
        snapshot.all_day_trades = true;
        for (const Instrument* instrument : {&kSze, &kSse}) {
            SnapshotPosition position;
            position.instrument = *instrument;
            position.total = 10000;
            position.free_sellable = 10000;
            snapshot.positions.push_back(position);
        }
        backend->publish(snapshot);
        require(engine->account().ready, "account ready");
    }

    Intent intent(const std::string& owner, const std::string& id,
                  const Instrument& instrument, Side side, Quantity quantity,
                  Money price = 100000) const {
        Intent value;
        value.owner = owner;
        value.intent_id = id;
        value.signal_id = id + "-signal";
        value.instrument = instrument;
        value.side = side;
        value.type = OrderType::Limit;
        value.price = price;
        value.quantity = quantity;
        return value;
    }

    Report order_report(OrderId id, const Instrument& instrument, Side side,
                        OrderState state, Quantity cumulative, Quantity leaves,
                        const std::string& broker = std::string()) const {
        Report report;
        report.scope = engine->scope();
        report.id = id;
        report.broker_id = broker.empty() ? ("B" + std::to_string(id)) : broker;
        report.instrument = instrument;
        report.side = side;
        report.kind = ReportKind::Order;
        report.state = state;
        report.cumulative = cumulative;
        report.leaves = leaves;
        return report;
    }

    Report trade_report(OrderId id, const Instrument& instrument, Side side,
                        const std::string& trade_id, Quantity quantity,
                        Quantity cumulative_after = -1) const {
        Report report = order_report(id, instrument, side, OrderState::Partial,
                                     -1, -1, "B" + std::to_string(id));
        report.kind = ReportKind::Trade;
        report.trade_id = trade_id;
        report.trade_quantity = quantity;
        report.cumulative_after = cumulative_after;
        report.trade_price = 100000;
        report.trade_fee = 0;
        report.fee_is_final = false;
        return report;
    }

    SubmitResult submit(const Intent& value) {
        return engine->submit(value);
    }
};

void attach_echo(Fixture& f) {
    f.backend->submit_hook = [&f](const Command& command) {
        SendResult result;
        result.disposition = SendDisposition::Submitted;
        if (!command.cancel) {
            f.backend->publish(f.order_report(command.id, command.intent.instrument,
                                              command.intent.side, OrderState::Accepted, 0,
                                              command.intent.quantity));
        }
        return result;
    };
}

// Identical Accepted report delivered twice must be a no-op: one audit note,
// no frozen account, no cash mutation.
void test_duplicate_identical_accepted() {
    Fixture f;
    attach_echo(f);
    SubmitResult order = f.submit(f.intent("client", "dup-acc", kSze, Side::Buy, 300));
    require(order.accepted, "order accepted");
    require(f.engine->report(f.order_report(order.id, kSze, Side::Buy, OrderState::Accepted, 0, 300)),
            "first accepted applied");
    require(f.engine->report(f.order_report(order.id, kSze, Side::Buy, OrderState::Accepted, 0, 300)),
            "identical duplicate accepted is tolerated");
    OrderView view;
    require(f.engine->order(order.id, &view) && view.state == OrderState::Accepted &&
            view.filled == 0 && view.working == 300, "duplicate accepted does not mutate order");
    require(f.engine->account().ready, "duplicate accepted does not freeze account");
}

// Identical Filled reports and identical trade reports delivered twice must
// settle cash/positions exactly once (cash settles on priced trade reports).
void test_duplicate_identical_filled() {
    Fixture f;
    attach_echo(f);
    SubmitResult order = f.submit(f.intent("client", "dup-fill", kSze, Side::Buy, 300));
    const Money before = f.engine->account().cash_balance;
    Report trade = f.trade_report(order.id, kSze, Side::Buy, "T-300", 300, 300);
    require(f.engine->report(trade), "first priced trade applied");
    require(f.engine->report(trade), "identical duplicate trade is tolerated");
    Report filled = f.order_report(order.id, kSze, Side::Buy, OrderState::Filled, 300, 0);
    require(f.engine->report(filled), "first filled applied");
    require(f.engine->report(filled), "identical duplicate filled is tolerated");
    OrderView view;
    require(f.engine->order(order.id, &view) && view.terminal && view.filled == 300,
            "duplicate filled keeps terminal state");
    require(f.engine->account().cash_balance == before - 30000000LL &&
            f.engine->account().ready, "fill cash applied exactly once");
}

// A filled status repeated after an equal terminal trade must not double-count.
void test_late_terminal_repeat_after_trade() {
    Fixture f;
    attach_echo(f);
    SubmitResult order = f.submit(f.intent("client", "late-term", kSze, Side::Buy, 300));
    f.backend->publish(f.trade_report(order.id, kSze, Side::Buy, "T-300", 300, 300));
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::Partial, 300, 0));
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::Filled, 300, 0));
    OrderView view;
    require(f.engine->order(order.id, &view) && view.terminal && view.filled == 300,
            "terminal repeat is consistent");
    require(f.engine->account().ready, "terminal repeat does not freeze");
}

// has_working_order transitions: false -> (submit) true -> (filled) false.
void test_has_working_order_transitions() {
    Fixture f;
    attach_echo(f);
    SubmitResult order = f.submit(f.intent("client", "hw", kSze, Side::Buy, 300));
    require(order.accepted && f.engine->has_working_order(kSze), "working after accepted");
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::Filled, 300, 0));
    require(!f.engine->has_working_order(kSze), "no working after filled");
    SubmitResult other = f.submit(f.intent("client", "hw-c", kSse, Side::Buy, 100));
    f.backend->publish(f.order_report(other.id, kSse, Side::Buy, OrderState::Canceled, 0, 100));
    require(!f.engine->has_working_order(kSse), "no working after canceled");
}

// CancelResult published after the order is already terminal is ignored.
void test_cancel_result_after_terminal() {
    Fixture f;
    attach_echo(f);
    SubmitResult order = f.submit(f.intent("client", "late-cx", kSze, Side::Buy, 300));
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::Filled, 300, 0));
    Report cancel_result;
    cancel_result.scope = f.engine->scope();
    cancel_result.id = order.id;
    cancel_result.instrument = kSze;
    cancel_result.side = Side::Buy;
    cancel_result.kind = ReportKind::CancelResult;
    require(f.engine->report(cancel_result), "late cancel result ignored");
    OrderView view;
    require(f.engine->order(order.id, &view) && view.terminal && view.filled == 300,
            "late cancel result leaves terminal order intact");
}

// AtpBackend closed loop: bind sinks, echo a report through the sender, assert
// the sink sees the report once, then disconnect is delivered.
void test_atp_backend_closed_loop() {
    Config config;
    config.scope.account.broker = "guoxin";
    config.scope.account.account = "acc-1";
    config.scope.gateway = "atp";
    config.scope.day = 20260904;
    config.scope.source = 190;
    config.scope.epoch = 1;
    config.instance = "atp-loopback";
    const Scope scope = config.scope;
    int order_calls = 0;
    std::atomic<int> reports(0), disconnects(0);
    std::shared_ptr<AtpBackend> backend(new AtpBackend(scope,
        [&order_calls, &backend, &reports](const Command& command) {
            ++order_calls;
            SendResult result;
            result.disposition = SendDisposition::Submitted;
            if (!command.cancel) {
                Report report;
                report.scope = command.scope;
                report.id = command.id;
                report.broker_id = std::to_string(command.id);
                report.instrument = command.intent.instrument;
                report.side = command.intent.side;
                report.kind = ReportKind::Order;
                report.state = OrderState::Accepted;
                report.original = command.intent.quantity;
                report.cumulative = 0;
                report.leaves = command.intent.quantity;
                backend->publish(report);
                ++reports;
            }
            return result;
        }));
    std::atomic<bool> report_seen(false);
    backend->bind([&report_seen](const Report&) { report_seen.store(true); },
                  [](const Snapshot&) {}, [&disconnects](const Scope&, bool) { ++disconnects; });
    Command submit;
    submit.scope = scope;
    submit.id = 7;
    submit.cancel = false;
    Intent intent;
    intent.owner = "strategy";
    intent.intent_id = "i7";
    intent.instrument = kSse;
    intent.side = Side::Buy;
    intent.quantity = 100;
    intent.price = 10000000;
    submit.intent = intent;
    SendResult sent = backend->submit(submit);
    require(sent.disposition == SendDisposition::Submitted && order_calls == 1 && reports == 1,
            "closed-loop echo reached the sink");
    require(report_seen.load(), "OMS report sink received the echo");
    backend->disconnected();
    require(disconnects.load() == 1, "OMS connection sink received disconnect");
}

// OMS-3 restart-cancel: a fresh reconcile that restores a working order
// cancels it first; account is not ready (and new submits are rejected) until
// the restored order reaches terminal.
void test_restart_cancel_open_orders() {
    Fixture f(true);
    f.backend->submit_hook = [&f](const Command& command) {
        SendResult result;
        result.disposition = SendDisposition::Submitted;
        if (!command.cancel) {
            f.backend->publish(f.order_report(command.id, command.intent.instrument,
                                              command.intent.side, OrderState::Accepted, 0,
                                              command.intent.quantity));
        }
        return result;
    };
    // First reconcile is empty and ready; place a live working order.
    SubmitResult order = f.submit(f.intent("client", "restart-open", kSze, Side::Buy, 300));
    require(order.accepted && f.engine->account().ready, "initial order accepted and ready");
    // Simulate restart: new reconcile whose broker snapshot restores the same
    // owned working order (id, owner, broker identity and state preserved).
    require(f.engine->begin_reconcile(2, false), "restart reconcile begins");
    Snapshot restart;
    restart.scope = f.engine->scope();
    restart.token = 2;
    restart.free_cash = 1000000000000LL;
    restart.account_success = true;
    restart.positions_success = true;
    restart.orders_success = true;
    restart.trades_success = true;
    restart.all_day_orders = true;
    restart.all_day_trades = true;
    SnapshotOrder restored;
    restored.id = order.id;
    restored.owner = "client";
    restored.broker_id = "B" + std::to_string(order.id);
    restored.instrument = kSze;
    restored.side = Side::Buy;
    restored.price = 100000;
    restored.original = 300;
    restored.filled = 0;
    restored.working = 300;
    restored.state = OrderState::Accepted;
    restart.orders.push_back(restored);
    SnapshotPosition position;
    position.instrument = kSze;
    position.total = 10000;
    position.free_sellable = 10000;
    restart.positions.push_back(position);
    SnapshotPosition sse_position;
    sse_position.instrument = kSse;
    sse_position.total = 10000;
    sse_position.free_sellable = 10000;
    restart.positions.push_back(sse_position);
    require(f.engine->complete_snapshot(restart), "restart snapshot installed");
    require(!f.engine->account().ready, "restart-cancel keeps account not ready");
    f.engine->advance_to(1);
    SubmitResult blocked = f.submit(f.intent("client", "during-restart-cancel", kSse, Side::Buy, 100));
    require(!blocked.accepted && blocked.error.category == ErrorCategory::NotReady,
            "new orders rejected while restored order awaits cancel");
    // Broker cancel returns terminal; only then may trading resume.
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::Canceled, 0, 300,
                                      "B" + std::to_string(order.id)));
    require(f.engine->account().ready && !f.engine->has_working_order(kSze),
            "ready resumes after restored order is canceled");
}

// OMS-3b certified query: AtpBackend advertises complete_snapshot only when a
// query sender is installed; query forwards to it; publish_snapshot reaches
// the OMS snapshot sink.
void test_atp_backend_certified_query() {
    Config config;
    config.scope.account.broker = "guoxin";
    config.scope.account.account = "acc-1";
    config.scope.gateway = "atp";
    config.scope.day = 20260904;
    config.scope.source = 190;
    config.scope.epoch = 1;
    const Scope scope = config.scope;
    int query_calls = 0;
    std::atomic<bool> snapshot_seen(false);
    std::shared_ptr<AtpBackend> backend(new AtpBackend(scope,
        [](const Command&) { SendResult r; r.disposition = SendDisposition::Submitted; return r; },
        [&query_calls](const Scope&, std::uint64_t) { ++query_calls; return Error(); },
        true));
    require(backend->capabilities().complete_snapshot, "certified query advertises complete snapshots");
    backend->bind([](const Report&) {}, [&snapshot_seen](const Snapshot&) { snapshot_seen.store(true); },
                  [](const Scope&, bool) {});
    Error query = backend->query(scope, 42);
    require(!query.failed() && query_calls == 1, "certified query forwarded once");
    Snapshot snapshot;
    snapshot.scope = scope;
    backend->publish_snapshot(snapshot);
    require(snapshot_seen.load(), "broker snapshot reaches the OMS sink");
}

// OMS-3b engine reconcile: begin_reconcile with request_backend dispatches a
// query; once the broker snapshot is published the account reconciles ready.
void test_engine_query_then_snapshot_reconcile() {
    Fixture f;
    int query_calls = 0;
    f.backend->query_hook = [&query_calls](const Scope&, std::uint64_t) { ++query_calls; return Error(); };
    require(f.engine->begin_reconcile(2, true), "request backend reconcile begins");
    require(query_calls == 1, "engine requested the certified broker query");
    require(!f.engine->account().ready, "account waits for broker snapshot");
    Snapshot broker;
    broker.scope = f.engine->scope();
    broker.token = 2;
    broker.free_cash = 1000000000000LL;
    broker.account_success = true;
    broker.positions_success = true;
    broker.orders_success = true;
    broker.trades_success = true;
    broker.all_day_orders = true;
    broker.all_day_trades = true;
    SnapshotPosition sze_position;
    sze_position.instrument = kSze;
    sze_position.total = 10000;
    sze_position.free_sellable = 10000;
    broker.positions.push_back(sze_position);
    SnapshotPosition sse_position;
    sse_position.instrument = kSse;
    sse_position.total = 10000;
    sse_position.free_sellable = 10000;
    broker.positions.push_back(sse_position);
    f.backend->publish(broker);
    require(f.engine->account().ready, "account ready after certified broker snapshot");
}

}  // namespace

int main() {
    try {
        test_duplicate_identical_accepted();
        test_duplicate_identical_filled();
        test_late_terminal_repeat_after_trade();
        test_has_working_order_transitions();
        test_cancel_result_after_terminal();
        test_atp_backend_closed_loop();
        test_restart_cancel_open_orders();
        test_atp_backend_certified_query();
        test_engine_query_then_snapshot_reconcile();
    } catch (const std::exception& error) {
        std::cerr << "oms_merge_test FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "oms_merge_test: ok\n";
    return 0;
}
