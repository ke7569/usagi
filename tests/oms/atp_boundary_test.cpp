#include "adapters/td/atp/OmsAtpBackend.h"
#include "common/oms/Oms.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace oms;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

Scope scope() {
    Scope value;
    value.account.broker = "broker";
    value.account.account = "account";
    value.gateway = "atp-fixture";
    value.day = 20260904;
    value.epoch = 7;
    value.source = 190;
    return value;
}

Config config(const Scope& value) {
    Config result;
    result.scope = value;
    result.instance = "atp-boundary";
    result.enabled = true;
    result.ownership = OwnershipMode::Simulation;
    InstrumentRules rules;
    rules.tick = 100;
    rules.lot = 100;
    rules.lower_price = 90000;
    rules.upper_price = 110000;
    result.instruments[Instrument{"SZE", "000001"}] = rules;
    return result;
}

Command command(const Scope& value, bool cancel = false,
                OrderType type = OrderType::Limit) {
    Command result;
    result.scope = value;
    result.id = 17;
    result.cancel = cancel;
    result.intent.owner = "owner";
    result.intent.intent_id = "intent-17";
    result.intent.signal_id = "signal-17";
    result.intent.instrument = Instrument{"SZE", "000001"};
    result.intent.side = Side::Buy;
    result.intent.type = type;
    result.intent.price = 100000;
    result.intent.quantity = 100;
    return result;
}

void test_real_capabilities_and_simulation_rejection() {
    const Scope expected = scope();
    AtpBackend backend(expected, [](const Command&) { return SendResult(); });
    const Capabilities caps = backend.capabilities();
    require(!caps.simulated && !caps.complete_snapshot && !caps.native_fak &&
            caps.query_reconcile && !caps.trades_required_for_snapshot,
            "ATP backend advertises current-state query recovery without uncertified history coverage");
    bool rejected = false;
    try {
        std::shared_ptr<Engine> engine = Engine::create(config(expected),
            std::shared_ptr<Backend>(new AtpBackend(expected, [](const Command&) { return SendResult(); })));
        (void)engine;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "real ATP backend cannot use simulation ownership");
}

void test_scope_cancel_and_native_fak_guards() {
    const Scope expected = scope();
    int sends = 0;
    AtpBackend backend(expected, [&sends](const Command&) {
        ++sends;
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    });
    Command wrong_epoch = command(expected);
    wrong_epoch.scope.epoch++;
    require(backend.submit(wrong_epoch).error.category == ErrorCategory::Unsupported,
            "wrong epoch rejected");
    Command wrong_account = command(expected);
    wrong_account.scope.account.account = "other";
    require(backend.submit(wrong_account).error.category == ErrorCategory::Unsupported,
            "wrong account rejected");
    Command wrong_cancel = command(expected, true);
    require(backend.submit(wrong_cancel).error.category == ErrorCategory::Unsupported,
            "submit with cancel flag rejected");
    require(backend.cancel(command(expected)).error.category == ErrorCategory::Unsupported,
            "cancel without cancel flag rejected");
    require(backend.submit(command(expected, false, OrderType::NativeFak)).error.category == ErrorCategory::Unsupported,
            "native FAK rejected");
    require(sends == 0, "invalid commands never reach sender");
}

void test_sender_dispositions_raw_error_and_close() {
    const Scope expected = scope();
    int sends = 0;
    AtpBackend backend(expected, [&sends](const Command&) {
        ++sends;
        SendResult result;
        if (sends == 1) result.disposition = SendDisposition::Submitted;
        else if (sends == 2) result.disposition = SendDisposition::Unknown;
        else result.disposition = SendDisposition::NotSent;
        result.error.category = ErrorCategory::Protocol;
        result.error.raw_code = 27001;
        result.error.raw_type = "fixture";
        return result;
    });
    const SendResult submitted = backend.submit(command(expected));
    const SendResult unknown = backend.submit(command(expected));
    const SendResult not_sent = backend.submit(command(expected));
    require(sends == 3, "valid commands reach controlled sender");
    require(submitted.disposition == SendDisposition::Submitted && submitted.error.raw_code == 27001,
            "submitted disposition/raw error preserved");
    require(unknown.disposition == SendDisposition::Unknown && unknown.error.raw_type == "fixture",
            "unknown disposition/raw type preserved");
    require(not_sent.disposition == SendDisposition::NotSent && not_sent.error.category == ErrorCategory::Protocol,
            "not-sent disposition preserved");

    backend.close();
    require(backend.submit(command(expected)).error.category == ErrorCategory::Unsupported,
            "closed backend rejects submit");
    require(backend.cancel(command(expected, true)).error.category == ErrorCategory::Unsupported,
            "closed backend rejects cancel");
    require(sends == 3, "closed backend does not call sender");
}

void test_publish_scope_and_connection_callback() {
    const Scope expected = scope();
    AtpBackend backend(expected, [](const Command&) { return SendResult(); });
    int reports = 0;
    int connected = 0;
    int disconnected = 0;
    backend.bind([&reports](const Report&) { ++reports; },
                 Backend::SnapshotSink(),
                 [&connected, &disconnected](const Scope& value, bool connected_state) {
                     require(value == scope(), "connection scope changed");
                     if (connected_state) ++connected;
                     else ++disconnected;
                 });
    Report current;
    current.scope = expected;
    current.id = 17;
    Report foreign = current;
    foreign.scope.epoch++;
    backend.publish(foreign);
    backend.publish(current);
    require(reports == 1, "only current scope report published");
    backend.connected();
    backend.disconnected();
    require(connected == 1 && disconnected == 1, "connection callbacks report exact state");
    backend.unbind();
}

void test_query_dispatch_is_scope_and_token_correlated() {
    const Scope expected = scope();
    int dispatches = 0;
    Scope dispatched_scope;
    std::uint64_t dispatched_token = 0;
    int synchronous_snapshots = 0;
    AtpBackend* callback_backend = nullptr;
    AtpBackend backend(expected,
        [](const Command&) { return SendResult(); },
        [&dispatches, &dispatched_scope, &dispatched_token, &synchronous_snapshots, &callback_backend]
        (const Scope& value, std::uint64_t token) {
            ++dispatches;
            dispatched_scope = value;
            dispatched_token = token;
            if (token == 77 && callback_backend != nullptr) {
                Snapshot snapshot;
                snapshot.scope = value;
                snapshot.token = token;
                callback_backend->publish(snapshot);
                ++synchronous_snapshots;
            }
            Error result;
            if (token == 99) {
                result.category = ErrorCategory::Temporary;
                result.message = "interrupted fixture query";
            }
            return result;
        });
    callback_backend = &backend;
    backend.bind(Backend::ReportSink(), [&synchronous_snapshots](const Snapshot&) {
        ++synchronous_snapshots;
    });
    Error wrong = backend.query(Scope(), 9);
    require(wrong.category == ErrorCategory::Unsupported && dispatches == 0,
            "wrong query scope is rejected before dispatch");
    Error empty_token = backend.query(expected, 0);
    require(empty_token.category == ErrorCategory::Unsupported && dispatches == 0,
            "zero query token is rejected before dispatch");
    Error accepted = backend.query(expected, 41);
    require(!accepted.failed() && dispatches == 1 && dispatched_scope == expected && dispatched_token == 41,
            "query dispatch preserves exact scope and token");
    Error synchronous = backend.query(expected, 77);
    require(!synchronous.failed() && dispatches == 2 && synchronous_snapshots == 2,
            "synchronous query callback can publish while transport is serialized");
    Error interrupted = backend.query(expected, 99);
    require(interrupted.category == ErrorCategory::Temporary &&
            interrupted.message == "interrupted fixture query" && dispatches == 3,
            "query dispatch preserves asynchronous-start error");
    backend.close();
    require(backend.query(expected, 100).category == ErrorCategory::Unsupported && dispatches == 3,
            "closed query transport cannot dispatch a late generation");
    backend.unbind();
}

void test_snapshot_assembly_is_normalized_and_deduplicated() {
    const Scope expected = scope();
    AtpSnapshotAssembler assembler(expected, 41);
    require(assembler.set_free_cash(1234500), "fund query value accepted");

    SnapshotPosition position;
    position.instrument = Instrument{"SZE", "000001"};
    position.total = 100;
    position.free_sellable = 35;
    require(assembler.add_position(position), "position accepted");
    require(assembler.add_position(position), "identical position callback is idempotent");

    SnapshotOrder order;
    order.id = 0;
    order.owner = "external";
    order.broker_id = "7001";
    order.instrument = position.instrument;
    order.side = Side::Sell;
    order.price = 1000000;
    order.original = 40;
    order.filled = 10;
    order.working = 30;
    order.state = OrderState::Partial;
    require(assembler.add_order(order), "working order accepted");

    Report trade;
    trade.scope = expected;
    trade.id = 0;
    trade.broker_id = "7001";
    trade.instrument = position.instrument;
    trade.side = Side::Sell;
    trade.kind = ReportKind::Trade;
    trade.trade_id = "exec-1";
    trade.trade_quantity = 10;
    trade.trade_price = 1000000;
    trade.trade_fee = 100;
    trade.fee_is_final = false;
    require(assembler.add_trade(trade), "trade accepted");
    require(assembler.add_trade(trade), "duplicate trade callback is idempotent");

    const Snapshot assembled = assembler.snapshot(true, true, true, true, false, false);
    require(assembled.scope == expected && assembled.token == 41 && assembled.free_cash == 1234500,
            "snapshot keeps OMS correlation");
    require(assembled.positions.size() == 1 && assembled.positions[0].free_sellable == 35,
            "available shares are not reduced by working sell twice");
    require(assembled.orders.size() == 1 && assembled.orders[0].working == 30 &&
            assembled.orders[0].id == 0,
            "working external order keeps ownership-unproven id zero");
    require(assembled.trades.size() == 1 && assembled.trades[0].id == 0 &&
            !assembled.trades[0].fee_is_final,
            "duplicate external trade identity is collapsed without fee finality");
    require(!assembled.all_day_orders && !assembled.all_day_trades,
            "all-day coverage is not inferred");

    Report same_exec_other_order = trade;
    same_exec_other_order.broker_id = "7002";
    require(assembler.add_trade(same_exec_other_order),
            "same execution identifier on another order remains distinct");
    require(assembler.snapshot(true, true, true, true, false, false).trades.size() == 2,
            "trade identity includes broker order identity");

    AtpSnapshotAssembler identity(expected, 44);
    require(identity.set_free_cash(1), "identity fixture fund row accepted");
    require(identity.add_trade(trade), "identity fixture trade accepted");
    Report foreign_trade = trade;
    foreign_trade.scope.epoch++;
    require(!identity.add_trade(foreign_trade), "foreign trade scope is rejected");

    AtpSnapshotAssembler empty(expected, 42);
    require(empty.set_free_cash(1), "empty query fund row accepted");
    const Snapshot empty_snapshot = empty.snapshot(true, true, true, true, false, false);
    require(empty_snapshot.account_success && empty_snapshot.positions_success &&
            empty_snapshot.orders_success && empty_snapshot.trades_success &&
            empty_snapshot.positions.empty() && empty_snapshot.orders.empty() &&
            empty_snapshot.trades.empty(), "empty terminal query produces an explicit empty snapshot");

    Report conflicting = trade;
    conflicting.trade_quantity = 9;
    require(!assembler.add_trade(conflicting), "conflicting duplicate trade is rejected");
    require(!assembler.set_free_cash(1234501), "conflicting fund rows are rejected");
    const Snapshot invalid = assembler.snapshot(true, true, true, true, false, false);
    require(!invalid.account_success && !invalid.trades_success,
            "assembly error closes snapshot success flags");

    AtpSnapshotAssembler invalid_side(expected, 43);
    require(invalid_side.set_free_cash(1), "invalid-side fixture fund row accepted");
    require(invalid_side.add_position(position), "invalid-side fixture position accepted");
    SnapshotOrder bad_order = order;
    bad_order.broker_id = "7003";
    bad_order.side = static_cast<Side>(99);
    require(!invalid_side.add_order(bad_order), "unsupported order side is rejected");
    Report bad_trade = trade;
    bad_trade.trade_id = "exec-bad-side";
    bad_trade.side = static_cast<Side>(99);
    require(!invalid_side.add_trade(bad_trade), "unsupported trade side is rejected");
    const Snapshot invalid_side_snapshot = invalid_side.snapshot(true, true, true, true, false, false);
    require(!invalid_side_snapshot.orders_success, "invalid order side closes order success");
}

void test_snapshot_publish_rejects_stale_scope() {
    const Scope expected = scope();
    AtpBackend backend(expected, [](const Command&) { return SendResult(); });
    int snapshots = 0;
    backend.bind(Backend::ReportSink(), [&snapshots](const Snapshot&) { ++snapshots; });
    Snapshot stale;
    stale.scope = expected;
    stale.scope.epoch++;
    stale.token = 1;
    backend.publish(stale);
    Snapshot current = stale;
    current.scope = expected;
    backend.publish(current);
    require(snapshots == 1, "stale snapshot callback is ignored");
    backend.unbind();
}

}  // namespace

int main() {
    try {
        test_real_capabilities_and_simulation_rejection();
        test_scope_cancel_and_native_fak_guards();
        test_sender_dispositions_raw_error_and_close();
        test_publish_scope_and_connection_callback();
        test_query_dispatch_is_scope_and_token_correlated();
        test_snapshot_assembly_is_normalized_and_deduplicated();
        test_snapshot_publish_rejects_stale_scope();
    } catch (const std::exception& error) {
        std::cerr << "atp_boundary_test: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
