#ifndef USAGI_TEST_OMS_EXECUTION_H
#define USAGI_TEST_OMS_EXECUTION_H

#include "common/execution/OmsStrategyExecution.h"
#include "common/oms/Oms.h"

#include <memory>
#include <string>

namespace oms_test {

struct ManagedFixture {
    std::shared_ptr<oms::ScriptedBackend> backend;
    std::shared_ptr<oms::Engine> engine;
    std::shared_ptr<strategy_runtime::OmsStrategyExecution> execution;

    ManagedFixture(const nlohmann::json& legacy, const std::string& market,
                   short source, const std::string& owner = "session-test") {
        oms::Config config;
        config.scope.account.broker = "paper";
        config.scope.account.account = owner;
        config.scope.gateway = "session-fixture";
        config.scope.day = 20260904;
        config.scope.source = source;
        config.instance = owner;
        config.enabled = true;
        config.ownership = oms::OwnershipMode::Simulation;
        oms::Capabilities capabilities;
        capabilities.simulated = true;
        capabilities.complete_snapshot = true;
        capabilities.fills = oms::FillCoverage::DualFromOrigin;
        capabilities.trades_required_for_snapshot = true;
        backend.reset(new oms::ScriptedBackend(capabilities));
        backend->submit_hook = [](const oms::Command& command) {
            oms::SendResult result;
            result.disposition = oms::SendDisposition::Submitted;
            result.broker_id = "B" + std::to_string(command.id);
            return result;
        };
        backend->cancel_hook = [](const oms::Command&) {
            oms::SendResult result;
            result.disposition = oms::SendDisposition::Submitted;
            return result;
        };
        const std::string suffix = market == "SH" ? ".SH" : ".SZ";
        const std::string exchange = market == "SH" ? "SSE" : "SZE";
        oms::Snapshot snapshot;
        snapshot.free_cash = 1000000000000LL;
        snapshot.account_success = snapshot.positions_success = true;
        snapshot.orders_success = snapshot.trades_success = true;
        snapshot.all_day_orders = snapshot.all_day_trades = true;
        for (nlohmann::json::const_iterator it = legacy.at("ins_params").begin();
             it != legacy.at("ins_params").end(); ++it) {
            const std::string raw = it.key();
            if (raw.size() < suffix.size() || raw.substr(raw.size() - suffix.size()) != suffix)
                continue;
            const std::string code = raw.substr(0, raw.size() - suffix.size());
            oms::Instrument instrument = {exchange, code};
            oms::InstrumentRules rules;
            rules.tick = 100;
            rules.lot = it.value().value("vol_unit", 100);
            rules.lower_price = 10000;
            rules.upper_price = 10000000;
            config.instruments[instrument] = rules;
            oms::SnapshotPosition position;
            position.instrument = instrument;
            position.total = it.value().value("static_position", 0) +
                             it.value().value("last_position", 0);
            position.free_sellable = position.total;
            snapshot.positions.push_back(position);
        }
        engine = oms::Engine::create(config, backend);
        if (!engine->start_epoch(1) || !engine->begin_reconcile(1, false))
            throw std::runtime_error("managed fixture reconciliation setup failed");
        snapshot.scope = engine->scope();
        snapshot.token = 1;
        if (!engine->complete_snapshot(snapshot))
            throw std::runtime_error("managed fixture snapshot failed");
        execution.reset(new strategy_runtime::OmsStrategyExecution(engine, owner));
    }
};

}  // namespace oms_test

#endif
