#include "common/execution/AccountReconciliation.h"
#include "common/strategy/StrategySession.h"
#include "tests/oms/TestExecution.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool value, const char* expression, int line) {
    if (!value) {
        std::ostringstream message;
        message << "line " << line << ": " << expression;
        throw std::runtime_error(message.str());
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

template <typename Exception, typename Function>
void check_throws(Function action, const char* expression, int line) {
    try { action(); }
    catch (const Exception&) { return; }
    check(false, expression, line);
}
#define CHECK_THROWS(type, expression) \
    check_throws<type>([&]() { expression; }, "expected " #type ": " #expression, __LINE__)

using strategy_runtime::AccountIdentity;
using strategy_runtime::AccountReconciliation;
using strategy_runtime::StrategySession;

AccountIdentity identity(short source = 88) {
    AccountIdentity value = {"COMMON-ACCOUNT", source, 20260904U};
    return value;
}

nlohmann::json config(const std::string& market) {
    nlohmann::json value;
    value["market"] = market;
    value["global_params"]["offset"] = 1.0;
    value["global_params"]["global_bias_factor"] = 1.0;
    value["global_params"]["position_base_line"] = 100000.0;
    value["global_params"]["position_limit"] = 1000.0;
    value["sze_startup_warmup_signals"] = 0;
    if (market == "SH") {
        value["ins_params"]["600000.SH"]["static_position"] = 1000;
        value["ins_params"]["600000.SH"]["last_position"] = 0;
        value["ins_params"]["600001.SH"]["static_position"] = 1000;
        value["ins_params"]["600001.SH"]["last_position"] = 0;
    } else {
        value["ins_params"]["000001.SZ"]["static_position"] = 1000;
        value["ins_params"]["000001.SZ"]["last_position"] = 0;
    }
    return value;
}

MSMarketDataField view(double bid, double ask) {
    MSMarketDataField value = {MSMarketData()};
    value.BidPrice1 = bid;
    value.AskPrice1 = ask;
    value.BidVolume1 = 100000.0;
    value.AskVolume1 = 100000.0;
    value.LastPrice = (bid + ask) * 0.5;
    value.MidPrice = value.LastPrice;
    value.MarketTime = 93000000.0;
    value.Volume = 100000.0;
    value.Turnover = 1000000.0;
    return value;
}

void begin(AccountReconciliation& gate, std::uint64_t epoch = 1,
           std::uint64_t token = 1) {
    CHECK(gate.begin_epoch(epoch));
    CHECK(gate.mark_connected(epoch, true));
    CHECK(gate.begin_snapshot(epoch, token));
}

void complete(AccountReconciliation& gate, std::uint64_t epoch = 1,
              std::uint64_t token = 1) {
    const AccountIdentity account = identity();
    CHECK(gate.add_account(epoch, token, account, 100000.0));
    CHECK(gate.add_position(epoch, token, account, "600000", 1000, 1000));
    CHECK(gate.add_position(epoch, token, account, "600001", 1000, 1000));
    CHECK(gate.finish_positions(epoch, token));
    CHECK(gate.finish_orders(epoch, token, 0));
    CHECK(gate.finish_snapshot(epoch, token));
    CHECK(gate.ready());
}

std::map<std::string, std::pair<int, int> > positions(
    const AccountReconciliation::Positions& source) {
    std::map<std::string, std::pair<int, int> > result;
    for (AccountReconciliation::Positions::const_iterator it = source.begin();
         it != source.end(); ++it)
        result[it->first] = std::make_pair(it->second.total, it->second.available);
    return result;
}

void test_reconciliation_gate_and_complete_bootstrap() {
    oms_test::ManagedFixture managed(config("SH"), "SH", 88);
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    bool healthy = true;
    StrategySession session(config("SH"), "SH", 88, backend,
                            [&healthy]() { return healthy; });
    CHECK(!session.execution()->permits_new_orders());

    AccountReconciliation gate(identity(), {"600000", "600001"});
    begin(gate);
    complete(gate);
    CHECK(gate.ready());
    session.sync_startup_positions(positions(gate.positions()));
    CHECK(!session.execution()->permits_new_orders());
    session.set_ready(true, true, true);
    CHECK(session.execution()->permits_new_orders());

    const MSMarketDataField book = view(10.0, 10.1);
    managed.engine->advance_to(1000000000LL);
    session.on_signal("600000", book, 100.0, 1000000000L);
    managed.engine->advance_to(2000000000LL);
    session.on_signal("600000", book, 100.0, 2000000000L);
    CHECK(managed.engine->account().admissions > 0);
}

void test_reconciliation_failures_keep_gate_closed() {
    AccountReconciliation missing(identity(), {"600000", "600001"});
    begin(missing);
    CHECK(missing.add_account(1, 1, identity(), 1.0));
    CHECK(missing.add_position(1, 1, identity(), "600000", 1, 1));
    CHECK(!missing.finish_positions(1, 1));
    CHECK(!missing.ready());

    AccountReconciliation open_orders(identity(), {"600000", "600001"});
    begin(open_orders);
    CHECK(open_orders.add_account(1, 1, identity(), 1.0));
    CHECK(open_orders.add_position(1, 1, identity(), "600000", 1, 1));
    CHECK(open_orders.add_position(1, 1, identity(), "600001", 1, 1));
    CHECK(open_orders.finish_positions(1, 1));
    CHECK(!open_orders.finish_orders(1, 1, 1));
    CHECK(!open_orders.ready());

    AccountReconciliation stale(identity(), {"600000", "600001"});
    CHECK(stale.begin_epoch(1));
    CHECK(!stale.mark_connected(2, true));
    CHECK(!stale.begin_snapshot(2, 1));
    CHECK(!stale.ready());

    oms_test::ManagedFixture managed(config("SH"), "SH", 88);
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    StrategySession session(config("SH"), "SH", 88, backend, []() { return true; });
    CHECK_THROWS(std::runtime_error, session.sync_startup_positions(
        std::map<std::string, std::pair<int, int> >{{"600000", std::make_pair(1, 1)}}));
    CHECK(!session.execution()->permits_new_orders());
}

void test_full_map_validation_and_one_shot_sync() {
    oms_test::ManagedFixture managed(config("SH"), "SH", 88);
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    StrategySession session(config("SH"), "SH", 88, backend, []() { return true; });
    std::map<std::string, std::pair<int, int> > incomplete;
    incomplete["600000"] = std::make_pair(1000, 800);
    incomplete["600001"] = std::make_pair(-1, 0);
    CHECK_THROWS(std::runtime_error, session.sync_startup_positions(incomplete));

    oms_test::ManagedFixture bad_managed(config("SH"), "SH", 88, "bad-session");
    std::shared_ptr<StrategyExecution> bad_backend = bad_managed.execution;
    StrategySession bad_session(config("SH"), "SH", 88, bad_backend,
                                []() { return true; });
    std::map<std::string, std::pair<int, int> > partial;
    partial["600000"] = std::make_pair(0, 0);
    partial["600001"] = std::make_pair(1000, 1001);
    CHECK_THROWS(std::runtime_error, bad_session.sync_startup_positions(partial));
    bad_session.set_ready(true, true, true);
    bad_session.on_signal("600000", view(10.0, 10.1), 100.0, 1L);
    bad_session.on_signal("600000", view(10.0, 10.1), 100.0, 2L);
    CHECK(bad_managed.engine->account().admissions > 0);  // Invalid map did not partially alter 600000 state.

    std::map<std::string, std::pair<int, int> > complete_positions;
    complete_positions["600000"] = std::make_pair(1000, 1000);
    complete_positions["600001"] = std::make_pair(1000, 1000);
    session.sync_startup_positions(complete_positions);

    session.set_ready(false, false, false);
    session.on_signal("600000", view(10.0, 10.1), 0.0, 1L);
    CHECK_THROWS(std::runtime_error, session.sync_startup_positions(complete_positions));
    CHECK(!session.execution()->permits_new_orders());
}

void test_market_and_numeric_configuration_boundaries() {
    oms_test::ManagedFixture managed(config("SH"), "SH", 88);
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    nlohmann::json wrong_sh = config("SH");
    wrong_sh["ins_params"].erase("600001.SH");
    wrong_sh["ins_params"]["000001.SZ"] = wrong_sh["ins_params"]["600000.SH"];
    CHECK_THROWS(std::runtime_error, StrategySession(
        wrong_sh, "SH", 88, backend, []() { return true; }));
    CHECK_THROWS(std::runtime_error, StrategySession(
        config("SZ"), "SH", 88, backend, []() { return true; }));

    nlohmann::json nan_config = config("SH");
    nan_config["global_params"]["offset"] =
        std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS(std::runtime_error, StrategySession(
        nan_config, "SH", 88, backend, []() { return true; }));

    nlohmann::json huge_position = config("SH");
    huge_position["ins_params"]["600000.SH"]["static_position"] =
        1.0e30;
    CHECK_THROWS(std::runtime_error, StrategySession(
        huge_position, "SH", 88, backend, []() { return true; }));
}

}  // namespace

int main() {
    try {
        test_reconciliation_gate_and_complete_bootstrap();
        test_reconciliation_failures_keep_gate_closed();
        test_full_map_validation_and_one_shot_sync();
        test_market_and_numeric_configuration_boundaries();
    } catch (const std::exception& error) {
        std::cerr << "strategy_session_test: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "strategy_session_test: PASS" << std::endl;
    return 0;
}
