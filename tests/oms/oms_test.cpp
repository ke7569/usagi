#include "common/oms/Oms.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace oms;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

const Instrument kSze = {"SZE", "000001"};
const Instrument kSse = {"SSE", "600000"};

struct TempJournal {
    std::string directory;
    std::string path;
    TempJournal() {
        char value[] = "/tmp/usagi-oms-hot-path-XXXXXX";
        char* created = ::mkdtemp(value);
        require(created != 0, "mkdtemp");
        directory = created;
        path = directory + "/account.journal";
    }
    ~TempJournal() {
        ::unlink(path.c_str());
        ::unlink((directory + "/oms-7061706572-756e6974.lock").c_str());
        ::rmdir(directory.c_str());
    }
};

struct Fixture {
    Config config;
    Capabilities capabilities;
    std::shared_ptr<ScriptedBackend> backend;
    std::shared_ptr<Engine> engine;

    explicit Fixture(const std::function<void(Config&)>& customize = std::function<void(Config&)>(),
                     const std::function<void(Capabilities&)>& transport = std::function<void(Capabilities&)>()) {
        config.scope.account.broker = "paper";
        config.scope.account.account = "unit";
        config.scope.gateway = "fixture";
        config.scope.day = 20260904;
        config.scope.source = 1;
        config.instance = "test";
        config.enabled = true;
        config.ownership = OwnershipMode::Simulation;
        config.instruments[kSze] = rules();
        config.instruments[kSse] = rules();
        if (customize) customize(config);
        capabilities.simulated = true;
        capabilities.complete_snapshot = true;
        capabilities.fills = FillCoverage::DualFromOrigin;
        capabilities.trades_required_for_snapshot = true;
        if (transport) transport(capabilities);
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
        SnapshotPosition sze_position;
        sze_position.instrument = kSze;
        sze_position.total = 10000;
        sze_position.free_sellable = 10000;
        snapshot.positions.push_back(sze_position);
        SnapshotPosition sse_position;
        sse_position.instrument = kSse;
        sse_position.total = 10000;
        sse_position.free_sellable = 10000;
        snapshot.positions.push_back(sse_position);
        backend->publish(snapshot);
        require(engine->account().ready, "account ready");
    }

    static InstrumentRules rules() {
        InstrumentRules result;
        result.tick = 100;
        result.lot = 100;
        result.lower_price = 90000;
        result.upper_price = 110000;
        return result;
    }

    Intent intent(const std::string& owner, const std::string& id,
                  const Instrument& instrument, Side side, Quantity quantity,
                  OrderType type = OrderType::Limit, Money price = 100000) const {
        Intent value;
        value.owner = owner;
        value.intent_id = id;
        value.signal_id = id + "-signal";
        value.instrument = instrument;
        value.side = side;
        value.type = type;
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
                        Quantity cumulative_after = -1,
                        const std::string& broker = std::string()) const {
        Report report = order_report(id, instrument, side, OrderState::Partial,
                                     -1, -1, broker);
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

void test_shared_budget_and_two_clients() {
    Fixture f;
    f.backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    SubmitResult a = f.submit(f.intent("sze-client", "a", kSze, Side::Buy, 500));
    SubmitResult b = f.submit(f.intent("sse-client", "b", kSse, Side::Buy, 500));
    require(a.accepted && b.accepted, "both markets accepted within shared cash");
    AccountView account = f.engine->account();
    require(account.pending_orders == 2, "shared pending count");
    require(account.cash_reserved == 100000000, "shared cash reservation");
}

void test_duplicate_owner_and_intent() {
    Fixture f;
    std::atomic<int> calls(0);
    f.backend->submit_hook = [&calls](const Command&) {
        ++calls; SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    Intent value = f.intent("client", "same", kSze, Side::Buy, 100);
    SubmitResult first = f.submit(value);
    SubmitResult duplicate = f.submit(value);
    require(first.accepted && !duplicate.accepted, "duplicate intent rejected");
    require(duplicate.error.category == ErrorCategory::Duplicate && calls == 1,
            "duplicate does not call backend");
    Intent other_owner = value; other_owner.owner = "other";
    SubmitResult other = f.submit(other_owner);
    require(other.accepted, "owner is part of duplicate intent identity");
    Error wrong_cancel = f.engine->cancel("other", first.id);
    require(wrong_cancel.category == ErrorCategory::Ownership,
            "owner cannot cancel another owner's order");
}

void test_cumulative_regression() {
    Fixture f;
    f.backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    SubmitResult submitted = f.submit(f.intent("client", "cum", kSze, Side::Buy, 1000));
    require(submitted.accepted, "cumulative order accepted");
    f.backend->publish(f.order_report(submitted.id, kSze, Side::Buy, OrderState::Partial, 300, 700));
    f.backend->publish(f.order_report(submitted.id, kSze, Side::Buy, OrderState::Partial, 0, 1000));
    f.backend->publish(f.order_report(submitted.id, kSze, Side::Buy, OrderState::Partial, 300, 700));
    OrderView view;
    require(f.engine->order(submitted.id, &view), "cumulative order view");
    require(view.filled == 300 && view.working == 700, "cumulative high water mark");
}

void test_dual_order_trade_dedup() {
    Fixture f;
    f.backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    SubmitResult submitted = f.submit(f.intent("client", "trade", kSze, Side::Buy, 1000));
    f.backend->publish(f.order_report(submitted.id, kSze, Side::Buy, OrderState::Partial, 300, 700));
    f.backend->publish(f.trade_report(submitted.id, kSze, Side::Buy, "T300", 300));
    f.backend->publish(f.trade_report(submitted.id, kSze, Side::Buy, "T300", 300));
    f.backend->publish(f.trade_report(submitted.id, kSze, Side::Buy, "T700", 700, 1000));
    OrderView view;
    require(f.engine->order(submitted.id, &view), "trade order view");
    require(view.filled == 1000 && view.working == 0 && view.terminal,
            "dual order and trade streams count once");
}

void test_late_trade_does_not_release_other_order() {
    Fixture f;
    f.backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    SubmitResult a = f.submit(f.intent("client", "late-a", kSze, Side::Buy, 300));
    SubmitResult b = f.submit(f.intent("client", "late-b", kSze, Side::Buy, 500));
    f.backend->publish(f.order_report(a.id, kSze, Side::Buy, OrderState::Filled, 300, 0));
    f.backend->publish(f.trade_report(a.id, kSze, Side::Buy, "late", 100, 400));
    OrderView b_view;
    require(f.engine->order(b.id, &b_view) && b_view.working == 500,
            "late trade cannot release B");
    require(!f.engine->account().ready, "contradictory late trade freezes account");
}

void test_unknown_send_retains_exposure() {
    Fixture f;
    f.backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Unknown; return result;
    };
    SubmitResult unknown = f.submit(f.intent("client", "unknown", kSze, Side::Buy, 1000));
    require(unknown.accepted, "unknown send is retained, not ordinary reject");
    OrderView view;
    require(f.engine->order(unknown.id, &view) && view.state == OrderState::Unknown && view.working == 1000,
            "unknown send keeps working reservation");
    SubmitResult blocked = f.submit(f.intent("client", "after-unknown", kSze, Side::Buy, 100));
    require(!blocked.accepted && blocked.error.category == ErrorCategory::NotReady,
            "unknown send blocks new risk");
}

void test_reentrant_and_threaded_callbacks() {
    Fixture f;
    std::atomic<int> calls(0);
    f.backend->submit_hook = [&f, &calls](const Command& command) {
        ++calls;
        std::thread callback([&f, command]() {
            f.backend->publish(f.order_report(command.id, command.intent.instrument,
                                              command.intent.side, OrderState::Accepted, 0,
                                              command.intent.quantity));
        });
        callback.join();
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    SubmitResult sync = f.submit(f.intent("client", "sync", kSze, Side::Buy, 100));
    require(sync.accepted && calls == 1, "synchronous callback is registered");
    SubmitResult async = f.submit(f.intent("client", "thread", kSse, Side::Buy, 100));
    std::thread publisher([&f, async]() {
        f.backend->publish(f.order_report(async.id, kSse, Side::Buy, OrderState::Accepted, 0, 100));
    });
    publisher.join();
    OrderView view;
    require(f.engine->order(async.id, &view) && view.state == OrderState::Accepted,
            "thread callback completes without deadlock");
}

void test_self_cross_and_cancel_pending() {
    Fixture f;
    f.backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    SubmitResult buy = f.submit(f.intent("client", "cross-buy", kSze, Side::Buy, 100));
    SubmitResult sell = f.submit(f.intent("client", "cross-sell", kSze, Side::Sell, 100));
    require(buy.accepted && !sell.accepted && sell.error.category == ErrorCategory::SelfTrade,
            "crossing own working orders rejected");
    f.backend->publish(f.order_report(buy.id, kSze, Side::Buy, OrderState::Accepted, 0, 100));
    int cancel_calls = 0;
    f.backend->cancel_hook = [&cancel_calls](const Command&) {
        ++cancel_calls; SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    require(f.engine->cancel("client", buy.id).category == ErrorCategory::None,
            "cancel accepted");
    require(f.engine->cancel("client", buy.id).category == ErrorCategory::None && cancel_calls == 1,
            "duplicate cancel coalesced");
}

void test_missing_id_cancel_and_timeout_retry() {
    Fixture f;
    f.backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    SubmitResult order = f.submit(f.intent("client", "missing-id", kSze, Side::Buy, 100));
    int cancel_calls = 0;
    f.backend->cancel_hook = [&cancel_calls](const Command&) {
        ++cancel_calls; SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    require(f.engine->cancel("client", order.id).category == ErrorCategory::None,
            "missing ID cancel remains pending");
    require(cancel_calls == 0, "missing ID does not dispatch cancel");
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::Accepted, 0, 100,
                                      "B" + std::to_string(order.id)));
    f.engine->advance_to(999999999LL);
    require(cancel_calls == 1, "ID arrival dispatches one cancel");
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::CancelPending, 0, 100,
                                      "B" + std::to_string(order.id)));
    f.engine->advance_to(1100000000LL);
    require(static_cast<unsigned>(cancel_calls) <= f.config.limits.max_cancel_attempts, "cancel retry bounded");
}

void test_terminal_timer_stop_and_limits() {
    Fixture f;
    f.backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    SubmitResult order = f.submit(f.intent("client", "timer", kSze, Side::Buy, 100));
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::Filled, 100, 0));
    int cancels = 0;
    f.backend->cancel_hook = [&cancels](const Command&) {
        ++cancels; SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    require(!f.engine->schedule_cancel("client", order.id, 100), "terminal timer not scheduled");
    f.engine->advance_to(1000);
    require(cancels == 0, "terminal timer does not cancel");
    f.engine->begin_stop();
    SubmitResult stopped = f.submit(f.intent("client", "stopped", kSse, Side::Buy, 100));
    require(!stopped.accepted && stopped.error.category == ErrorCategory::NotReady,
            "stop blocks new orders");
}

void test_capacity_rate_and_stale_scope() {
    Fixture f([](Config& config) {
        config.limits.max_pending = 1;
        config.limits.max_orders = 1;
        config.limits.new_orders_per_window = 1;
    });
    f.backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    SubmitResult first = f.submit(f.intent("client", "limit-1", kSze, Side::Buy, 100));
    SubmitResult second = f.submit(f.intent("client", "limit-2", kSse, Side::Buy, 100));
    require(first.accepted && !second.accepted, "capacity and rate reject excess");
    Report stale = f.order_report(first.id, kSze, Side::Buy, OrderState::Filled, 100, 0);
    stale.scope.epoch = 2;
    require(!f.engine->report(stale), "stale epoch ignored");
    Report foreign = stale;
    foreign.scope = f.engine->scope();
    foreign.scope.account.account = "foreign";
    require(!f.engine->report(foreign), "foreign account ignored");
    OrderView view;
    require(f.engine->order(first.id, &view) && view.filled == 0,
            "stale/foreign reports do not mutate order");
}

void test_money_unknown_price_and_fee_completion() {
    Fixture f([](Config& config) { config.limits.fee_reserve_per_order = 10000; });
    f.backend->submit_hook = [](const Command&) {
        SendResult r; r.disposition = SendDisposition::Submitted; return r;
    };
    const Money initial = f.engine->account().cash_balance;
    const auto sent = f.submit(f.intent("cash", "buy", kSze, Side::Buy, 1000));
    require(sent.accepted && f.engine->account().cash_reserved == 100010000, "buy principal and fee pre-reserved");
    f.backend->publish(f.order_report(sent.id, kSze, Side::Buy, OrderState::Partial, 300, 700));
    OrderView view;
    require(f.engine->order(sent.id, &view) && !view.amount_complete && !view.fees_complete, "quantity is not price or fee proof");
    require(f.engine->account().cash_balance == initial && f.engine->account().cash_reserved == 100010000,
            "unpriced fills retain principal without inventing cash execution");
    Report trade = f.trade_report(sent.id, kSze, Side::Buy, "priced-300", 300, 300);
    trade.trade_price = 99000; trade.trade_fee = -1;
    f.backend->publish(trade);
    require(f.engine->account().cash_balance == initial - 29700000 && f.engine->account().cash_reserved == 70010000,
            "actual execution price debits cash once");
    f.backend->publish(f.order_report(sent.id, kSze, Side::Buy, OrderState::Canceled, 300, 0));
    require(f.engine->account().cash_reserved == 10000, "cancel retains unknown fee budget");
    trade.trade_fee = 3000; trade.fee_is_final = true;
    f.backend->publish(trade); f.backend->publish(trade);
    require(f.engine->order(sent.id, &view) && view.fees_complete && view.amount_complete,
            "duplicate trade may fill missing fee metadata");
    require(f.engine->account().cash_reserved == 0 && f.engine->account().cash_balance == initial - 29703000,
            "actual fees applied exactly once");
    Position position;
    require(f.engine->position(kSze, &position) && position.total == 10300 && position.sellable == 10000,
            "same-day bought shares do not increase T+1 sellable");
}

void test_not_sent_rsp_reject_and_cancel_retry() {
    Fixture f;
    f.backend->submit_hook = [](const Command&) {
        SendResult r; r.error.category = ErrorCategory::Rejected; r.error.raw_code = 27; return r;
    };
    const auto rejected = f.submit(f.intent("c", "reject", kSze, Side::Buy, 100));
    require(!rejected.accepted && rejected.error.raw_code == 27 && f.engine->account().cash_reserved == 0,
            "NotSent releases only its own risk and preserves raw error");
    f.backend->submit_hook = [](const Command&) { SendResult r; r.disposition = SendDisposition::Submitted; return r; };
    const auto active = f.submit(f.intent("c", "active", kSze, Side::Buy, 100));
    Report reject = f.order_report(active.id, kSze, Side::Buy, OrderState::Rejected, 0, 0);
    f.backend->publish(reject); f.backend->publish(reject);
    require(f.engine->account().pending_orders == 0 && f.engine->account().cash_reserved == 0,
            "Rsp-only and repeated rejection release once");
    const auto cancel = f.submit(f.intent("c", "cancel-retry", kSse, Side::Buy, 100));
    f.backend->publish(f.order_report(cancel.id, kSse, Side::Buy, OrderState::Accepted, 0, 100));
    unsigned calls = 0;
    f.backend->cancel_hook = [&calls](const Command&) {
        ++calls; SendResult r; r.error.category = ErrorCategory::Temporary; return r;
    };
    require(!f.engine->cancel("c", cancel.id).failed(), "cancel intent accepted");
    for (unsigned i = 1; i <= 10; ++i) f.engine->advance_to(i * f.config.limits.cancel_retry_ns);
    OrderView view;
    require(f.engine->order(cancel.id, &view) && view.cancel_exhausted && view.working == 100 &&
            calls == f.config.limits.max_cancel_attempts, "temporary cancel retries bounded and retain risk");
    f.engine->begin_stop();
    f.backend->publish(f.order_report(cancel.id, kSse, Side::Buy, OrderState::Canceled, 0, 0));
    require(f.engine->order(cancel.id, &view) && view.terminal && !view.working, "stop still accepts final reports");
}

void test_shared_cash_sellable_and_atomic_concurrency() {
    Fixture f([](Config& config) { config.instruments[kSze].max_order_notional = 1000000000000LL; });
    require(f.engine->begin_reconcile(2, false), "lower cash query");
    Snapshot snapshot; snapshot.scope = f.engine->scope(); snapshot.token = 2; snapshot.free_cash = 15000000;
    snapshot.account_success = snapshot.positions_success = snapshot.orders_success = snapshot.trades_success = true;
    snapshot.all_day_orders = snapshot.all_day_trades = true;
    for (const Instrument& instrument : {kSze, kSse}) {
        SnapshotPosition p; p.instrument = instrument; p.total = p.free_sellable = 10000; snapshot.positions.push_back(p);
    }
    require(f.engine->complete_snapshot(snapshot), "lower cash snapshot");
    std::atomic<unsigned> accepted(0);
    std::thread a([&]() { if (f.submit(f.intent("a", "buy", kSze, Side::Buy, 100)).accepted) ++accepted; });
    std::thread b([&]() { if (f.submit(f.intent("b", "buy", kSse, Side::Buy, 100)).accepted) ++accepted; });
    a.join(); b.join();
    require(accepted == 1 && f.engine->account().cash_reserved == 10000000, "concurrent markets cannot spend same cash twice");
    Fixture sell;
    const auto first = sell.submit(sell.intent("a", "sell", kSze, Side::Sell, 9900));
    const auto second = sell.submit(sell.intent("b", "sell", kSze, Side::Sell, 200));
    require(first.accepted && !second.accepted && second.error.category == ErrorCategory::Shares,
            "clients cannot sell the same shares twice");
}

void test_backend_rebind_does_not_detach_owner() {
    Fixture f;
    bool threw = false;
    try { Engine::create(f.config, f.backend); } catch (const std::logic_error&) { threw = true; }
    require(threw, "backend rejects a second OMS owner");
    auto sent = f.submit(f.intent("c", "still-bound", kSze, Side::Buy, 100));
    f.backend->publish(f.order_report(sent.id, kSze, Side::Buy, OrderState::Filled, 100, 0, "scripted-" + std::to_string(sent.id)));
    OrderView view;
    require(f.engine->order(sent.id, &view) && view.terminal, "failed rebind preserves original callback binding");
}

void test_coverage_capability_and_cancel_clock() {
    Fixture ambiguous(std::function<void(Config&)>(), [](Capabilities& c) { c.fills = FillCoverage::UnknownOverlap; });
    auto sent = ambiguous.submit(ambiguous.intent("c", "unknown-overlap", kSze, Side::Buy, 1000));
    const std::string broker = "scripted-" + std::to_string(sent.id);
    ambiguous.backend->publish(ambiguous.order_report(sent.id, kSze, Side::Buy, OrderState::Partial, 100, 900, broker));
    ambiguous.backend->publish(ambiguous.trade_report(sent.id, kSze, Side::Buy, "maybe-overlap", 100, -1, broker));
    OrderView view;
    require(ambiguous.engine->order(sent.id, &view) && !view.quantity_complete && !ambiguous.engine->account().ready,
            "unproven overlap never silently adds cumulative and trade quantities");
    Fixture f;
    auto native = f.submit(f.intent("c", "native", kSze, Side::Buy, 100, OrderType::NativeFak));
    require(!native.accepted && native.error.category == ErrorCategory::Unsupported, "unsupported native FAK rejected");
    Intent timed = f.intent("c", "accepted-clock", kSze, Side::Buy, 100, OrderType::LimitThenCancel);
    timed.cancel_clock = CancelClock::Acceptance; timed.cancel_delay_ns = 100;
    auto order = f.submit(timed);
    unsigned calls = 0;
    f.backend->cancel_hook = [&calls](const Command&) { ++calls; SendResult r; r.disposition = SendDisposition::Submitted; return r; };
    f.engine->advance_to(1000);
    require(calls == 0 && f.engine->account().timers == 0, "acceptance clock has not started on submission");
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::Accepted, 0, 100, "scripted-" + std::to_string(order.id)));
    f.engine->advance_to(1099); require(calls == 0, "acceptance timer not early");
    f.engine->advance_to(1100); require(calls == 1, "acceptance timer uses actual acceptance time");
}

void test_orphan_and_bounded_identity_storage() {
    Fixture f([](Config& c) { c.limits.max_trade_ids = 1; c.limits.max_orphans = 1; });
    f.backend->submit_hook = [&f](const Command& c) {
        Report early = f.order_report(0, c.intent.instrument, c.intent.side, OrderState::Accepted, 0, c.intent.quantity, "EARLY");
        f.backend->publish(early);
        SendResult r; r.disposition = SendDisposition::Submitted; r.broker_id = "EARLY"; return r;
    };
    auto sent = f.submit(f.intent("c", "orphan-first", kSze, Side::Buy, 1000));
    require(sent.accepted && f.engine->account().orphans == 0 && f.engine->account().ready,
            "early broker-ID-only report is resolved when submit returns its ID");
    f.backend->publish(f.trade_report(sent.id, kSze, Side::Buy, "one", 100, 100, "EARLY"));
    f.backend->publish(f.trade_report(sent.id, kSze, Side::Buy, "two", 100, 200, "EARLY"));
    require(f.engine->account().trade_ids == 1 && !f.engine->account().ready, "trade identity capacity closes new risk");
    Fixture orphan([](Config& c) { c.limits.max_orphans = 1; });
    orphan.backend->publish(orphan.order_report(99, kSze, Side::Buy, OrderState::Accepted, 0, 100, "NONE"));
    require(orphan.engine->account().orphans == 1 && !orphan.engine->account().ready, "unmatched report quarantines account");
    orphan.engine->advance_to(orphan.config.limits.orphan_timeout_ns);
    require(orphan.engine->account().orphans == 0 && !orphan.engine->account().ready, "expired orphan remains a reconciliation fault");
}

void test_terminal_cannot_contradict_trade_first_quantity() {
    Fixture f;
    const auto sent = f.submit(f.intent("c", "trade-before-reject", kSze, Side::Buy, 1000));
    const std::string broker = "scripted-" + std::to_string(sent.id);
    f.backend->publish(f.trade_report(sent.id, kSze, Side::Buy, "confirmed", 300, 300, broker));
    f.backend->publish(f.order_report(sent.id, kSze, Side::Buy, OrderState::Rejected, 0, 0, broker));
    OrderView view;
    require(f.engine->order(sent.id, &view) && view.filled == 300 && !f.engine->account().ready,
            "zero-fill terminal cannot erase a confirmed trade or reopen risk");
}

void test_async_log_saturation_preserves_cancel_and_reports() {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    Fixture f([&](Config& config) {
        config.durable_intent = false;
        config.audit_sink = [&](const std::string&) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true; changed.notify_all(); changed.wait(lock, [&]() { return release; });
        };
    });
    struct Release {
        std::mutex& mutex; std::condition_variable& changed; bool& flag;
        ~Release() { std::lock_guard<std::mutex> lock(mutex); flag = true; changed.notify_all(); }
    } release_on_exit = {mutex, changed, release};
    require(!f.config.durable_intent, "fixture exercises default asynchronous intent policy");
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(2), [&]() { return entered; }), "text worker entered");
    }
    const auto order = f.submit(f.intent("c", "before-full", kSze, Side::Buy, 100));
    require(order.accepted, "slow log sink does not block sending");
    Intent invalid = f.intent("c", "bad-lot", kSze, Side::Buy, 99);
    for (unsigned i = 0; i < 8300 && f.engine->account().ready; ++i) f.engine->submit(invalid);
    require(!f.engine->account().ready && f.engine->account().journal_pending <= 8192,
            "async queue exhaustion closes gate with bounded occupancy");
    require(!f.engine->submit(f.intent("c", "after-full", kSze, Side::Buy, 100)).accepted, "full queue rejects new risk");
    unsigned cancels = 0;
    f.backend->cancel_hook = [&](const Command&) { ++cancels; SendResult r; r.disposition = SendDisposition::Submitted; return r; };
    require(!f.engine->cancel("c", order.id).failed() && cancels == 1, "full/blocked logger cannot block known-order cancellation");
    f.backend->publish(f.order_report(order.id, kSze, Side::Buy, OrderState::Canceled, 0, 0,
                                     "scripted-" + std::to_string(order.id)));
    OrderView view;
    require(f.engine->order(order.id, &view) && view.terminal && !view.working,
            "reports still update account after async logging fails");
}

void test_durable_intent_override_waits_for_wal() {
    TempJournal temp;
    Fixture f([&](Config& config) {
        config.ownership = OwnershipMode::ExclusiveLocal;
        config.journal_path = temp.path;
        config.lock_directory = temp.directory;
        config.durable_intent = true;
    });
    const AccountView before = f.engine->account();
    const SubmitResult submitted = f.submit(f.intent("cold", "durable-intent", kSze, Side::Buy, 100));
    const AccountView after = f.engine->account();
    require(submitted.accepted && after.durable_sequence > before.durable_sequence,
            "durable_intent waits for the pre-send WAL barrier");
    require(after.durable_sequence <= after.audit_sequence,
            "durable watermark never exceeds accepted journal records");
}

}  // namespace

int main() {
    try {
        test_shared_budget_and_two_clients();
        test_duplicate_owner_and_intent();
        test_cumulative_regression();
        test_dual_order_trade_dedup();
        test_late_trade_does_not_release_other_order();
        test_unknown_send_retains_exposure();
        test_reentrant_and_threaded_callbacks();
        test_self_cross_and_cancel_pending();
        test_missing_id_cancel_and_timeout_retry();
        test_terminal_timer_stop_and_limits();
        test_capacity_rate_and_stale_scope();
        test_money_unknown_price_and_fee_completion();
        test_not_sent_rsp_reject_and_cancel_retry();
        test_shared_cash_sellable_and_atomic_concurrency();
        test_backend_rebind_does_not_detach_owner();
        test_coverage_capability_and_cancel_clock();
        test_orphan_and_bounded_identity_storage();
        test_terminal_cannot_contradict_trade_first_quantity();
        test_async_log_saturation_preserves_cancel_and_reports();
        test_durable_intent_override_waits_for_wal();
    } catch (const std::exception& error) {
        std::cerr << "oms_test: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
