#include "common/strategy/ZStrategy.h"
#include "common/execution/OmsStrategyExecution.h"

#include <iostream>
#include <limits>
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
    std::shared_ptr<oms::ScriptedBackend> backend;
    std::shared_ptr<oms::Engine> engine;
    std::vector<oms::Command> commands;

    Fixture() {
        config.scope.account.broker = "paper";
        config.scope.account.account = "unit";
        config.scope.gateway = "fixture";
        config.scope.day = 20260906;
        config.scope.source = 28;
        config.instance = "ztest";
        config.enabled = true;
        config.ownership = oms::OwnershipMode::Simulation;
        config.instruments[kInstrument] = rules();

        oms::Capabilities capabilities;
        capabilities.simulated = true;
        capabilities.complete_snapshot = true;
        capabilities.fills = oms::FillCoverage::DualFromOrigin;
        capabilities.trades_required_for_snapshot = true;
        backend.reset(new oms::ScriptedBackend(capabilities));
        backend->submit_hook = [this](const oms::Command& command) {
            commands.push_back(command);
            oms::SendResult result;
            result.disposition = oms::SendDisposition::Submitted;
            result.broker_id = "B" + std::to_string(command.id);
            return result;
        };
        backend->cancel_hook = [this](const oms::Command& command) {
            commands.push_back(command);
            oms::SendResult result;
            result.disposition = oms::SendDisposition::Submitted;
            return result;
        };
        engine = oms::Engine::create(config, backend);
        require(engine->start_epoch(1), "epoch");
        require(engine->begin_reconcile(1, false), "reconcile");

        oms::Snapshot snapshot;
        snapshot.scope = engine->scope();
        snapshot.token = 1;
        snapshot.free_cash = 1000000000000LL;
        snapshot.account_success = true;
        snapshot.positions_success = true;
        snapshot.orders_success = true;
        snapshot.trades_success = true;
        snapshot.all_day_orders = true;
        snapshot.all_day_trades = true;
        oms::SnapshotPosition position;
        position.instrument = kInstrument;
        position.total = 1000;
        position.free_sellable = 1000;
        snapshot.positions.push_back(position);
        require(engine->complete_snapshot(snapshot), "snapshot");
        require(engine->account().ready, "ready");
    }
};

nlohmann::json settings() {
    nlohmann::json value;
    value["market"] = "SH";
    value["td_source_index"] = nlohmann::json::array({28});
    value["sse_order_routing"]["enabled"] = true;
    value["sse_order_routing"]["mode"] = "live";
    value["sze_startup_warmup_signals"] = 0;
    value["global_params"]["offset"] = 1.0;
    value["global_params"]["global_bias_factor"] = 1.0;
    value["global_params"]["position_limit"] = 1000.0;
    value["global_params"]["position_base_line"] = 100000.0;
    value["ins_params"]["600000.SH"]["max_order_size"] = 100000.0;
    value["ins_params"]["600000.SH"]["min_order_size"] = 100.0;
    value["ins_params"]["600000.SH"]["vol_unit"] = 100;
    return value;
}

InsParams params() {
    InsParams value;
    value.static_position = 1000;
    return value;
}

MSMarketDataField book() {
    MSMarketDataField value = {};
    value.BidPrice1 = 10.0;
    value.AskPrice1 = 10.1;
    value.BidVolume1 = value.AskVolume1 = 100000;
    value.LastPrice = value.MidPrice = 10.05;
    value.MarketTime = 94000000.0;
    value.Volume = 100000;
    value.Turnover = 1000000;
    return value;
}

std::shared_ptr<strategy_runtime::OmsStrategyExecution> execution(const Fixture& fixture) {
    return std::shared_ptr<strategy_runtime::OmsStrategyExecution>(
        new strategy_runtime::OmsStrategyExecution(fixture.engine, "zstrategy"));
}

void test_submit_and_owned_cancel() {
    Fixture fixture;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> exec = execution(fixture);
    const int id = exec->submit_managed(28, "600000", "SSE", 10.0, 100, '0', '0',
                                        oms::OrderType::Limit, 0, std::function<bool()>());
    require(id > 0 && fixture.commands.size() == 1 && !fixture.commands[0].cancel, "submit");
    require(exec->owns_request(28, id, "600000"), "owned id");
    require(exec->cancel(28, id) == id, "cancel");
    require(fixture.commands.size() == 2 && fixture.commands.back().cancel, "backend cancel");
}

void test_fak_timer() {
    Fixture fixture;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> exec = execution(fixture);
    const int id = exec->submit_managed(28, "600000", "SSE", 10.0, 100, '0', '0',
                                        oms::OrderType::LimitThenCancel, 1001000000LL,
                                        std::function<bool()>());
    require(id > 0 && fixture.commands.size() == 1 && !fixture.commands[0].cancel, "FAK submit");
    fixture.engine->advance_to(1001000000LL - 1);
    require(fixture.commands.size() == 1, "timer early");
    fixture.engine->advance_to(1001000000LL);
    require(fixture.commands.size() == 2 && fixture.commands.back().cancel, "timer cancel");
}

void test_invalid_and_large_signals() {
    Fixture fixture;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> exec = execution(fixture);
    nlohmann::json config = settings();
    ZStrategy strategy("600000", params(), config, exec);
    MSMarketDataField market = book();
    strategy.on_signal(&market, 0.0, 28, 0);
    strategy.on_signal(&market, std::numeric_limits<double>::quiet_NaN(), 28, 0);
    strategy.on_signal(&market, std::numeric_limits<double>::infinity(), 28, 0);
    require(fixture.commands.empty(), "nonfinite");
    strategy.on_signal(&market, 1.0e15, 28, 0);
    require(fixture.commands.size() <= 1, "bounded signal");
}

void test_zstrategy_buy_and_sell_signal_paths() {
    Fixture buy_fixture;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> buy_exec = execution(buy_fixture);
    nlohmann::json buy_config = settings();
    ZStrategy buy_strategy("600000", params(), buy_config, buy_exec);
    MSMarketDataField buy_market = book();
    buy_strategy.on_signal(&buy_market, 100.0, 28, 0);
    require(buy_fixture.commands.empty(), "first signal is warmup");
    buy_strategy.on_signal(&buy_market, 100.0, 28, 0);
    require(!buy_fixture.commands.empty() && buy_fixture.commands[0].intent.signal_id == "600000:2:0",
            "OMS order links to actual instrument/sample/receive-time identity");
    require(buy_fixture.commands.size() == 1 && !buy_fixture.commands[0].cancel,
            "second buy signal submits");
    require(buy_fixture.commands[0].intent.side == oms::Side::Buy &&
                buy_fixture.commands[0].intent.quantity <= 9900,
            "buy quantity is bounded");

    Fixture sell_fixture;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> sell_exec = execution(sell_fixture);
    nlohmann::json sell_config = settings();
    ZStrategy sell_strategy("600000", params(), sell_config, sell_exec);
    MSMarketDataField sell_market = book();
    sell_strategy.on_signal(&sell_market, -100.0, 28, 0);
    require(sell_fixture.commands.empty(), "first sell signal is warmup");
    sell_strategy.on_signal(&sell_market, -100.0, 28, 0);
    require(sell_fixture.commands.size() == 1 &&
                sell_fixture.commands[0].intent.side == oms::Side::Sell,
            "second sell signal submits");
}

void test_risk_rejection_code_is_preserved() {
    Fixture fixture;
    unsigned attempts = 0;
    fixture.backend->submit_hook = [&attempts](const oms::Command&) {
        ++attempts;
        oms::SendResult result;
        result.disposition = oms::SendDisposition::NotSent;
        result.error.category = oms::ErrorCategory::RateLimited;
        result.error.raw_code = 2010;
        return result;
    };
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> exec = execution(fixture);
    const int result = exec->submit_managed(28, "600000", "SSE", 10.0, 100, '0', '0',
                                            oms::OrderType::Limit, 0, std::function<bool()>());
    require(result == -2010, "risk rejection code");
    nlohmann::json config = settings();
    ZStrategy strategy("600000", params(), config, exec);
    MSMarketDataField market = book();
    strategy.on_signal(&market, 100.0, 28, 0);
    strategy.on_signal(&market, 100.0, 28, 0);
    require(attempts == 2, "strategy actually reaches risk-reject backend");
    fixture.engine->advance_to(1);
    strategy.on_signal(&market, 100.0, 28, 0);
    require(attempts == 2, "risk cooldown suppresses subsequent strategy order");
    fixture.engine->advance_to(1000000000LL);
    strategy.on_signal(&market, 100.0, 28, 0);
    require(attempts == 3, "risk cooldown ends on explicit OMS time");
}

}  // namespace

int main() {
    try {
        test_submit_and_owned_cancel();
        test_fak_timer();
        test_invalid_and_large_signals();
        test_zstrategy_buy_and_sell_signal_paths();
        test_risk_rejection_code_is_preserved();
        std::cout << "zstrategy_execution_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "zstrategy_execution_test: " << error.what() << '\n';
        return 1;
    }
}
