#include "common/execution/StreamStrategyExecution.h"
#include "common/execution/OmsStrategyExecution.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

const oms::Instrument kInstrument = {"SSE", "600000"};

oms::InstrumentRules rules() {
    oms::InstrumentRules value;
    value.tick = 100;
    value.lot = 100;
    value.lower_price = 90000;
    value.upper_price = 110000;
    value.max_order_quantity = 100000;
    value.max_order_notional = 100000000000LL;
    return value;
}
struct Fixture {
    oms::Config config;
    std::shared_ptr<oms::PaperBackend> backend;
    std::shared_ptr<oms::Engine> engine;
    std::vector<oms::Command> commands;
    Fixture() {
        config.scope.account.broker = "paper";
        config.scope.account.account = "unit";
        config.scope.gateway = "fixture";
        config.scope.day = 20260906;
        config.scope.source = 190;
        config.instance = "stream-test";
        config.enabled = true;
        config.ownership = oms::OwnershipMode::Simulation;
        config.instruments[kInstrument] = rules();
        backend.reset(new oms::PaperBackend([this](const oms::Command& command) {
            commands.push_back(command);
        }));
        engine = oms::Engine::create(config, backend);
        require(engine->start_epoch(1), "epoch");
        require(engine->begin_reconcile(1, false), "reconcile");
        oms::Snapshot snapshot;
        snapshot.scope = engine->scope();
        snapshot.token = 1;
        snapshot.free_cash = 1000000000000LL;
        snapshot.account_success = snapshot.positions_success = snapshot.orders_success = true;
        snapshot.trades_success = snapshot.all_day_orders = snapshot.all_day_trades = true;
        oms::SnapshotPosition position;
        position.instrument = kInstrument;
        position.total = position.free_sellable = 1000;
        snapshot.positions.push_back(position);
        require(engine->complete_snapshot(snapshot), "snapshot");
        require(engine->account().ready, "ready");
    }
};
void test_unmanaged_rejected() {
    class UnmanagedExecution : public StrategyExecution {
    public:
        bool permits_new_orders() const override { return false; }
        long long now_ns() const override { return 0; }
        int submit_limit(short, const std::string&, const std::string&, double, int, char, char) override { return -1; }
        int cancel(short, int) override { return -1; }
        bool schedule_cancel(short, int, int) override { return false; }
    };
    std::shared_ptr<StrategyExecution> backend(new UnmanagedExecution());
    bool threw = false;
    try {
        strategy_runtime::ProtectedExecution execution(backend, []() { return true; }, 190, "SSE", {"600000"});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "unmanaged backend rejected");
}
void test_real_readiness_route_and_owned_id() {
    Fixture f;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> backend(
        new strategy_runtime::OmsStrategyExecution(f.engine, "stream"));
    strategy_runtime::ProtectedExecution protection(backend, []() { return true; }, 190, "SSE", {"600000"});
    require(!protection.permits_new_orders(), "closed initially");
    protection.set_ready(true, true, true);
    require(protection.permits_new_orders(), "ready");
    oms::Position position;
    require(protection.read_position(190, "600000", "SSE", &position) && position.total == 1000, "OMS position");
    const int id = protection.submit_limit(190, "600000", "SSE", 10.25, 100, '0', '0');
    require(id > 0 && f.commands.size() == 1 && !f.commands[0].cancel, "managed submit");
    require(protection.owns_request(190, id, "600000"), "owned id");
    require(protection.submit_limit(190, "600001", "SSE", 10.25, 100, '0', '0') < 0, "route instrument");
    require(protection.cancel(190, id) == id && f.commands.size() == 2 && f.commands.back().cancel, "owned cancel");
}

void test_health_stop_and_clocked_cancel() {
    Fixture f;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> backend(
        new strategy_runtime::OmsStrategyExecution(f.engine, "stream"));
    bool healthy = true;
    strategy_runtime::ProtectedExecution protection(backend, [&]() { return healthy; }, 190, "SSE", {"600000"});
    protection.set_ready(true, true, true);
    const int id = protection.submit_managed(190, "600000", "SSE", 10.25, 100, '0', '0',
        oms::OrderType::LimitThenCancel, 1001000000LL, std::function<bool()>());
    require(id > 0, "managed timer submit");
    healthy = false;
    protection.begin_stop();
    require(protection.submit_limit(190, "600000", "SSE", 10.25, 100, '0', '0') < 0, "stopped new order");
    f.engine->advance_to(1001000000LL - 1);
    require(f.commands.size() == 1, "cancel timer early");
    f.engine->advance_to(1001000000LL);
    require(f.commands.size() == 2 && f.commands.back().cancel, "cancel timer due");
}
void test_invalid_route_and_paper_no_fills() {
    Fixture f;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> b(
        new strategy_runtime::OmsStrategyExecution(f.engine, "stream"));
    strategy_runtime::ProtectedExecution p(b, []() { return true; }, 190, "SSE", {"600000"});
    p.set_ready(true, true, true);
    require(p.submit_limit(180, "600000", "SSE", 10.25, 100, '0', '0') < 0, "source");
    require(p.submit_limit(190, "600000", "SZE", 10.25, 100, '0', '0') < 0, "exchange");
    require(p.submit_limit(190, "600000", "SSE", 10.25, 0, '0', '0') < 0, "quantity");
    require(f.commands.size() == 0, "invalid not sent");
    const int id = p.submit_limit(190, "600000", "SSE", 10.25, 100, '0', '0');
    require(id > 0, "paper command");
    oms::OrderView order;
    oms::Position position;
    require(f.engine->order(id, &order) && order.filled == 0, "paper has no fill");
    require(f.engine->position(kInstrument, &position) && position.total == 1000, "paper position unchanged");
    require(p.schedule_cancel(190, id, 1001), "paper cancel schedule");
    f.engine->advance_to(1001000000LL);
    const std::size_t command_count = f.commands.size();
    require(command_count == 2 && f.commands.back().cancel, "paper cancel emitted");
    f.engine->advance_to(1001000000LL);
    require(f.commands.size() == command_count, "duplicate advance no cancel");
}

void test_signal_identity_belongs_to_each_order() {
    Fixture f;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> backend(
        new strategy_runtime::OmsStrategyExecution(f.engine, "stream"));
    strategy_runtime::ProtectedExecution protection(backend, []() { return true; },
        190, "SSE", {"600000"});
    protection.set_ready(true, true, true);

    std::string first = "600000:first", second = "600000:second";
    const int first_id = protection.submit_managed(190, "600000", "SSE", 10.25, 100, '0', '0',
        oms::OrderType::Limit, 0, std::function<bool()>(), first);
    const int second_id = protection.submit_managed(190, "600000", "SSE", 10.25, 100, '0', '0',
        oms::OrderType::Limit, 0, std::function<bool()>(), second);
    first.clear(); second.clear();
    const int without_signal = protection.submit_limit(190, "600000", "SSE", 10.25, 100, '0', '0');
    require(first_id > 0 && second_id > first_id && without_signal > second_id,
            "independent signal orders accepted");
    require(f.commands.size() == 3 &&
            f.commands[0].intent.signal_id == "600000:first" &&
            f.commands[1].intent.signal_id == "600000:second" &&
            f.commands[2].intent.signal_id.empty(),
            "orders own their signal identity; absent identity never inherits the previous order");
    oms::OrderView stored;
    require(f.engine->order(first_id, &stored) && stored.command.intent.signal_id == "600000:first",
            "OMS retains signal identity after caller storage changes");

    bool rejected = false;
    try {
        protection.submit_managed(190, "600000", "SSE", 10.25, 100, '0', '0',
            oms::OrderType::Limit, 0, std::function<bool()>(), std::string(129, 'x'));
    } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && f.commands.size() == 3, "oversized signal identity never reaches backend");
}
}
int main() {
    try {
        test_unmanaged_rejected();
        test_real_readiness_route_and_owned_id();
        test_health_stop_and_clocked_cancel();
        test_invalid_route_and_paper_no_fills();
        test_signal_identity_belongs_to_each_order();
        std::cout << "stream_strategy_execution_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stream_strategy_execution_test: " << error.what() << '\n';
        return 1;
    }
}
