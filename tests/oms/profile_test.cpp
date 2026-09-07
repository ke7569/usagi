#include "common/oms/Profile.h"
#include "common/oms/Oms.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::size_t index(oms::profile::Stage stage) {
    return static_cast<std::size_t>(stage);
}

void test_nested_scope_accounting() {
    oms::profile::Sample sample;
    {
        oms::profile::Session session(sample);
        oms::profile::Scope root(oms::profile::Stage::Submit);
        oms::profile::Scope child(oms::profile::Stage::Risk);
        child.stop();
        root.stop();
    }
    const std::size_t root = index(oms::profile::Stage::Submit);
    const std::size_t child = index(oms::profile::Stage::Risk);
    require(sample.calls[root] == 1 && sample.calls[child] == 1, "nested scope calls");
    require(sample.inclusive_ns[root] >= sample.inclusive_ns[child], "inclusive nesting");
    require(sample.exclusive_ns[root] + sample.inclusive_ns[child] == sample.inclusive_ns[root],
            "exclusive plus child accounting");
    std::uint64_t exclusive_total = 0;
    for (std::size_t i = 0; i < oms::profile::kStages; ++i) exclusive_total += sample.exclusive_ns[i];
    require(exclusive_total == sample.inclusive_ns[root], "exclusive total equals root inclusive");
}

void test_stop_idempotence_and_session_lifetime() {
    oms::profile::Sample sample;
    {
        oms::profile::Session session(sample);
        oms::profile::Scope scope(oms::profile::Stage::Gate);
        scope.stop();
        scope.stop();
    }
    const std::size_t gate = index(oms::profile::Stage::Gate);
    require(sample.calls[gate] == 1, "scope stop idempotent");
    const std::uint64_t calls = sample.calls[gate];
    {
        oms::profile::Scope outside(oms::profile::Stage::Gate);
        outside.stop();
    }
    require(sample.calls[gate] == calls, "session ended stops recording");
}

void test_thread_isolation() {
    oms::profile::Sample first;
    oms::profile::Sample second;
    std::thread a([&]() {
        oms::profile::Session session(first);
        oms::profile::Scope scope(oms::profile::Stage::Reservation);
        for (volatile int i = 0; i < 10000; ++i) {}
    });
    std::thread b([&]() {
        oms::profile::Session session(second);
        oms::profile::Scope scope(oms::profile::Stage::BackendCall);
        for (volatile int i = 0; i < 10000; ++i) {}
    });
    a.join();
    b.join();
    require(first.calls[index(oms::profile::Stage::Reservation)] == 1, "thread sample one");
    require(first.calls[index(oms::profile::Stage::BackendCall)] == 0, "thread sample isolation one");
    require(second.calls[index(oms::profile::Stage::BackendCall)] == 1, "thread sample two");
    require(second.calls[index(oms::profile::Stage::Reservation)] == 0, "thread sample isolation two");
}

void test_exception_raii() {
    oms::profile::Sample sample;
    try {
        oms::profile::Session session(sample);
        oms::profile::Scope scope(oms::profile::Stage::SendResult);
        throw std::runtime_error("expected");
    } catch (const std::runtime_error&) {}
    require(sample.calls[index(oms::profile::Stage::SendResult)] == 1, "exception scope RAII");
    oms::profile::Sample after;
    {
        oms::profile::Session session(after);
        oms::profile::Scope scope(oms::profile::Stage::FinalLookup);
    }
    require(after.calls[index(oms::profile::Stage::FinalLookup)] == 1, "session restored after exception");
}

void test_focused_scope() {
    oms::profile::Sample sample;
    {
        oms::profile::Session session(sample, oms::profile::only(oms::profile::Stage::Risk));
        oms::profile::Scope root(oms::profile::Stage::Submit);
        oms::profile::Scope selected(oms::profile::Stage::Risk);
    }
    require(sample.calls[index(oms::profile::Stage::Submit)] == 0, "unselected scope has no clock reads");
    require(sample.calls[index(oms::profile::Stage::Risk)] == 1, "focused scope recorded");
    oms::profile::Sample restored;
    {
        oms::profile::Session session(restored);
        oms::profile::Scope root(oms::profile::Stage::Submit);
    }
    require(restored.calls[index(oms::profile::Stage::Submit)] == 1, "focus mask restored after session");
}

struct Fixture {
    std::shared_ptr<oms::ScriptedBackend> backend;
    std::shared_ptr<oms::Engine> engine;
    Fixture() {
        oms::Config config;
        config.scope.account.broker = "paper";
        config.scope.account.account = "profile";
        config.scope.gateway = "profile-gateway";
        config.scope.day = 20260907;
        config.scope.source = 1;
        config.instance = "profile-test";
        config.enabled = true;
        config.ownership = oms::OwnershipMode::Simulation;
        oms::Instrument instrument = {"SSE", "600000"};
        oms::InstrumentRules rules;
        rules.tick = 100; rules.lot = 100; rules.lower_price = 90000; rules.upper_price = 110000;
        config.instruments[instrument] = rules;
        oms::Capabilities capabilities;
        capabilities.simulated = true;
        capabilities.complete_snapshot = true;
        capabilities.fills = oms::FillCoverage::DualFromOrigin;
        capabilities.trades_required_for_snapshot = true;
        backend.reset(new oms::ScriptedBackend(capabilities));
        engine = oms::Engine::create(config, backend);
        require(engine->start_epoch(1), "profile epoch");
        require(engine->begin_reconcile(1, false), "profile reconcile");
        oms::Snapshot snapshot;
        snapshot.scope = engine->scope(); snapshot.token = 1; snapshot.free_cash = 1000000000000LL;
        snapshot.account_success = snapshot.positions_success = snapshot.orders_success = true;
        snapshot.trades_success = snapshot.all_day_orders = snapshot.all_day_trades = true;
        oms::SnapshotPosition position;
        position.instrument = instrument; position.total = position.free_sellable = 10000;
        snapshot.positions.push_back(position);
        require(engine->complete_snapshot(snapshot), "profile snapshot");
    }
};

void test_engine_profile_path() {
    Fixture fixture;
    oms::Intent intent;
    intent.owner = "profile-owner"; intent.intent_id = "intent-1"; intent.signal_id = "signal-1";
    intent.instrument = oms::Instrument{"SSE", "600000"}; intent.side = oms::Side::Buy;
    intent.type = oms::OrderType::Limit; intent.price = 100000; intent.quantity = 100;
    oms::profile::Sample sample;
    oms::SubmitResult result;
    {
        oms::profile::Session session(sample);
        result = fixture.engine->submit(intent);
    }
    require(result.accepted, "profile engine submit");
    require(sample.calls[index(oms::profile::Stage::Submit)] == 1, "submit calls");
    require(sample.calls[index(oms::profile::Stage::Risk)] == 1, "risk calls");
    require(sample.calls[index(oms::profile::Stage::IntentAudit)] == 1, "intent audit calls");
    require(sample.calls[index(oms::profile::Stage::JournalAppend)] == 3, "journal append calls");
    require(sample.calls[index(oms::profile::Stage::JournalSync)] == 0, "memory journal sync calls");
    require(sample.calls[index(oms::profile::Stage::BackendCall)] == 1, "backend calls");
    std::uint64_t exclusive = 0;
    for (std::uint64_t value : sample.exclusive_ns) exclusive += value;
    require(exclusive == sample.inclusive_ns[index(oms::profile::Stage::Submit)], "actual submit exclusive sum");
    oms::OrderView order;
    oms::AccountView account = fixture.engine->account();
    require(fixture.engine->order(result.id, &order) && account.orders == 1 && account.pending_orders == 1,
            "profile order state");
}

void test_engine_rejection_has_no_backend_call() {
    Fixture fixture;
    require(fixture.engine->set_connected(fixture.engine->scope(), false), "disconnect for rejection");
    oms::Intent intent;
    intent.owner = "profile-owner"; intent.intent_id = "reject-1"; intent.signal_id = "reject-signal";
    intent.instrument = oms::Instrument{"SSE", "600000"}; intent.side = oms::Side::Buy;
    intent.type = oms::OrderType::Limit; intent.price = 100000; intent.quantity = 100;
    oms::profile::Sample sample;
    {
        oms::profile::Session session(sample);
        oms::SubmitResult result = fixture.engine->submit(intent);
        require(!result.accepted, "not-ready intent rejected");
    }
    require(sample.calls[index(oms::profile::Stage::BackendCall)] == 0, "admission rejection has no backend call");
}

void test_engine_not_sent_is_registered_without_fill() {
    Fixture fixture;
    fixture.backend->submit_hook = [](const oms::Command&) {
        oms::SendResult result;
        result.disposition = oms::SendDisposition::NotSent;
        result.error.category = oms::ErrorCategory::RateLimited;
        result.error.raw_code = 2010;
        return result;
    };
    oms::Intent intent;
    intent.owner = "profile-owner";
    intent.intent_id = "not-sent-1";
    intent.signal_id = "not-sent-signal";
    intent.instrument = oms::Instrument{"SSE", "600000"};
    intent.side = oms::Side::Buy;
    intent.type = oms::OrderType::Limit;
    intent.price = 100000;
    intent.quantity = 100;
    oms::profile::Sample sample;
    oms::SubmitResult result;
    {
        oms::profile::Session session(sample);
        result = fixture.engine->submit(intent);
    }
    require(!result.accepted && result.id != 0, "not-sent order remains identifiable");
    require(sample.calls[index(oms::profile::Stage::BackendCall)] == 1, "NotSent backend was called once");
    oms::OrderView view;
    require(fixture.engine->order(result.id, &view) && view.filled == 0, "not-sent has no fill");
}

void test_synchronous_callback_and_unknown_send() {
    Fixture fixture;
    fixture.backend->submit_hook = [&fixture](const oms::Command& command) {
        oms::Report report;
        report.scope = command.scope; report.id = command.id;
        report.instrument = command.intent.instrument; report.side = command.intent.side;
        report.original = command.intent.quantity; report.cumulative = 0; report.leaves = command.intent.quantity;
        fixture.backend->publish(report);
        oms::SendResult result; result.disposition = oms::SendDisposition::Submitted; return result;
    };
    oms::Intent intent;
    intent.owner = "profile-owner"; intent.intent_id = "sync";
    intent.instrument = oms::Instrument{"SSE", "600000"}; intent.price = 100000; intent.quantity = 100;
    oms::profile::Sample sample;
    {
        oms::profile::Session session(sample);
        require(fixture.engine->submit(intent).accepted, "synchronous callback submit");
    }
    require(sample.calls[index(oms::profile::Stage::JournalAppend)] == 4, "callback audit counted under parent");
    std::uint64_t exclusive = 0;
    for (std::uint64_t value : sample.exclusive_ns) exclusive += value;
    require(exclusive == sample.inclusive_ns[index(oms::profile::Stage::Submit)], "reentrant dispatch scope accounting");
    fixture.backend->submit_hook = [](const oms::Command&) {
        oms::SendResult result; result.disposition = oms::SendDisposition::Unknown; return result;
    };
    intent.intent_id = "unknown";
    oms::SubmitResult result;
    {
        oms::profile::Session session(sample);
        result = fixture.engine->submit(intent);
    }
    oms::OrderView view;
    require(result.accepted && fixture.engine->order(result.id, &view) && view.working == 100 &&
        view.state == oms::OrderState::Unknown && !fixture.engine->account().ready, "profiling retains unknown exposure");
}

}  // namespace

int main() {
    try {
        test_nested_scope_accounting();
        test_stop_idempotence_and_session_lifetime();
        test_thread_isolation();
        test_exception_raii();
        test_focused_scope();
        test_engine_profile_path();
        test_engine_rejection_has_no_backend_call();
        test_engine_not_sent_is_registered_without_fill();
        test_synchronous_callback_and_unknown_send();
        std::cout << "profile_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "profile_test: " << error.what() << '\n';
        return 1;
    }
}
