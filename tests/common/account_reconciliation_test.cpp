#include "common/execution/AccountReconciliation.h"

#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {

void check(bool result, const char* expression, int line) {
    if (!result) {
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

AccountIdentity identity() {
    const AccountIdentity result = {"ACCOUNT-A", 190, 20260904};
    return result;
}

AccountReconciliation make_gate() {
    return AccountReconciliation(identity(), {"600000", "600001"});
}

void start(AccountReconciliation& gate, std::uint64_t epoch = 1, std::uint64_t token = 1) {
    CHECK(gate.begin_epoch(epoch));
    CHECK(gate.mark_connected(epoch, true));
    CHECK(gate.begin_snapshot(epoch, token));
}

void rows(AccountReconciliation& gate, std::uint64_t epoch = 1, std::uint64_t token = 1) {
    CHECK(gate.add_account(epoch, token, identity(), 1234.5));
    CHECK(gate.add_position(epoch, token, identity(), "600000", 1000, 800));
    CHECK(gate.add_position(epoch, token, identity(), "600001", 0, 0));
}

void complete(AccountReconciliation& gate, std::uint64_t epoch = 1, std::uint64_t token = 1) {
    rows(gate, epoch, token);
    CHECK(gate.finish_positions(epoch, token));
    CHECK(gate.finish_orders(epoch, token, 0));
    CHECK(gate.finish_snapshot(epoch, token));
    CHECK(gate.ready());
}

void test_initial_gate_closed_and_invalid_construction() {
    AccountReconciliation gate = make_gate();
    CHECK(!gate.ready() && !gate.failure_reason().empty());
    CHECK_THROWS(std::logic_error, gate.positions());
    CHECK_THROWS(std::logic_error, gate.available_cash());
    CHECK(!gate.mark_connected(0, true));
    CHECK(!gate.begin_snapshot(1, 1));
    CHECK_THROWS(std::invalid_argument, AccountReconciliation(identity(), {}));
    CHECK_THROWS(std::invalid_argument, AccountReconciliation(identity(), {""}));
    for (unsigned field = 0; field < 3; ++field) {
        AccountIdentity invalid = identity();
        if (field == 0) invalid.account.clear();
        if (field == 1) invalid.source = 0;
        if (field == 2) invalid.day = 20260230;
        CHECK_THROWS(std::invalid_argument, AccountReconciliation(invalid, {"600000"}));
    }
}

void test_all_queries_and_final_marker_required() {
    AccountReconciliation gate = make_gate();
    CHECK(gate.begin_epoch(1));
    CHECK(!gate.begin_snapshot(1, 1));
    CHECK(gate.mark_connected(1, true));
    CHECK(!gate.ready());
    CHECK(gate.begin_snapshot(1, 1));
    CHECK(gate.add_account(1, 1, identity(), 0));
    CHECK(!gate.ready());
    CHECK(gate.add_position(1, 1, identity(), "600000", 1000, 800));
    CHECK(gate.add_position(1, 1, identity(), "600001", 0, 0));
    CHECK(!gate.ready());
    CHECK(gate.finish_positions(1, 1));
    CHECK(!gate.ready());
    CHECK(gate.finish_orders(1, 1, 0));
    CHECK(!gate.ready());
    CHECK_THROWS(std::logic_error, gate.positions());
    CHECK(gate.finish_snapshot(1, 1));
    CHECK(gate.ready() && gate.failure_reason().empty());
    CHECK(gate.positions().size() == 2);
    CHECK(gate.positions().at("600000").total == 1000);
    CHECK(gate.positions().at("600000").available == 800);
    CHECK(gate.positions().at("600001").total == 0);
    CHECK(gate.available_cash() == 0);
}

void test_independent_query_streams_may_arrive_in_any_order() {
    AccountReconciliation gate = make_gate();
    start(gate);
    CHECK(gate.finish_orders(1, 1, 0));
    CHECK(gate.add_position(1, 1, identity(), "600001", 0, 0));
    CHECK(gate.add_position(1, 1, identity(), "600000", 1000, 800));
    CHECK(gate.finish_positions(1, 1));
    CHECK(!gate.ready());
    CHECK(gate.add_account(1, 1, identity(), 1234.5));
    CHECK(gate.finish_snapshot(1, 1));
    CHECK(gate.ready());
}

void test_missing_positions_or_completions_never_imply_success() {
    for (unsigned missing = 0; missing < 4; ++missing) {
        AccountReconciliation gate = make_gate();
        start(gate);
        if (missing != 0) CHECK(gate.add_account(1, 1, identity(), 1));
        CHECK(gate.add_position(1, 1, identity(), "600000", 1, 1));
        if (missing != 1) CHECK(gate.add_position(1, 1, identity(), "600001", 0, 0));
        if (missing == 1) {
            CHECK(!gate.finish_positions(1, 1));
            CHECK(!gate.add_position(1, 1, identity(), "600001", 0, 0));
        } else if (missing != 2) {
            CHECK(gate.finish_positions(1, 1));
        }
        if (missing != 1 && missing != 3) CHECK(gate.finish_orders(1, 1, 0));
        CHECK(!gate.finish_snapshot(1, 1));
        CHECK(!gate.ready());
        CHECK(!gate.failure_reason().empty());
        CHECK_THROWS(std::logic_error, gate.positions());
    }
    AccountReconciliation empty = make_gate();
    start(empty);
    CHECK(!empty.finish_positions(1, 1));
    CHECK(!empty.ready());
}

void test_account_identity_mismatch_invalidates_only_current_snapshot() {
    for (unsigned field = 0; field < 3; ++field) {
        AccountIdentity wrong = identity();
        if (field == 0) wrong.account = "ACCOUNT-B";
        if (field == 1) wrong.source = 180;
        if (field == 2) wrong.day = 20260903;
        for (unsigned payload = 0; payload < 2; ++payload) {
            AccountReconciliation gate = make_gate();
            start(gate);
            if (payload == 0) CHECK(!gate.add_account(1, 1, wrong, 100));
            else CHECK(!gate.add_position(1, 1, wrong, "600000", 100, 100));
            CHECK(!gate.ready());
            CHECK(!gate.finish_snapshot(1, 1));
            CHECK(gate.begin_snapshot(1, 2));
            complete(gate, 1, 2);
        }
    }
}

void test_duplicate_rows_and_completion_markers_fail_closed() {
    for (unsigned duplicate = 0; duplicate < 5; ++duplicate) {
        AccountReconciliation gate = make_gate();
        start(gate);
        rows(gate);
        if (duplicate == 0) CHECK(!gate.add_account(1, 1, identity(), 1234.5));
        if (duplicate == 1) CHECK(!gate.add_position(1, 1, identity(), "600000", 1000, 800));
        if (duplicate >= 2) CHECK(gate.finish_positions(1, 1));
        if (duplicate == 2) CHECK(!gate.finish_positions(1, 1));
        if (duplicate >= 3) CHECK(gate.finish_orders(1, 1, 0));
        if (duplicate == 3) CHECK(!gate.finish_orders(1, 1, 0));
        if (duplicate == 4) {
            CHECK(gate.finish_snapshot(1, 1));
            CHECK(!gate.finish_snapshot(1, 1));
        }
        CHECK(!gate.ready());
        CHECK(!gate.failure_reason().empty());
    }
}

void test_invalid_cash_positions_and_unknown_instruments_are_rejected() {
    for (double cash : {-1.0, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
        AccountReconciliation gate = make_gate();
        start(gate);
        CHECK(!gate.add_account(1, 1, identity(), cash));
        CHECK(!gate.ready());
    }
    const int quantities[][2] = {{-1, 0}, {1, -1}, {1, 2}};
    for (const auto& value : quantities) {
        AccountReconciliation gate = make_gate();
        start(gate);
        CHECK(!gate.add_position(1, 1, identity(), "600000", value[0], value[1]));
        CHECK(!gate.ready());
    }
    AccountReconciliation gate = make_gate();
    start(gate);
    CHECK(!gate.add_position(1, 1, identity(), "600999", 0, 0));
    CHECK(!gate.finish_snapshot(1, 1));
}

void test_stale_epoch_and_token_cannot_pollute_current_snapshot() {
    AccountReconciliation gate = make_gate();
    start(gate);
    CHECK(gate.add_position(1, 1, identity(), "600000", 900, 900));
    CHECK(gate.begin_epoch(2));
    CHECK(gate.mark_connected(2, true));
    CHECK(gate.begin_snapshot(2, 2));
    const std::string reason = gate.failure_reason();
    CHECK(!gate.add_account(1, 1, identity(), 999999));
    CHECK(!gate.add_position(1, 1, identity(), "600000", 900, 900));
    CHECK(!gate.finish_positions(1, 1));
    CHECK(!gate.finish_orders(1, 1, 0));
    CHECK(!gate.finish_snapshot(1, 1));
    CHECK(!gate.mark_connected(1, false));
    CHECK(!gate.on_activity(1));
    CHECK(!gate.add_account(2, 1, identity(), 999999));
    CHECK(!gate.finish_orders(2, 3, 0));
    CHECK(gate.failure_reason() == reason && !gate.last_rejection().empty());
    complete(gate, 2, 2);
    CHECK(gate.available_cash() == 1234.5 && gate.positions().at("600000").total == 1000);
    CHECK(!gate.mark_connected(1, false));
    CHECK(!gate.finish_snapshot(2, 1));
    CHECK(gate.ready() && gate.failure_reason().empty());
}

void test_epoch_and_token_must_strictly_increase() {
    AccountReconciliation gate = make_gate();
    CHECK(!gate.begin_epoch(0));
    start(gate, 10, 20);
    CHECK(!gate.begin_epoch(10) && !gate.begin_epoch(9));
    CHECK(!gate.begin_snapshot(10, 20) && !gate.begin_snapshot(10, 19));
    complete(gate, 10, 20);
    CHECK(gate.begin_epoch(11));
    CHECK(!gate.ready());
    CHECK(gate.mark_connected(11, true));
    CHECK(!gate.begin_snapshot(11, 20));
    CHECK(gate.begin_snapshot(11, 21));
    complete(gate, 11, 21);
}

void test_new_snapshot_requires_new_rows_and_ignores_old_token() {
    AccountReconciliation gate = make_gate();
    start(gate);
    complete(gate);
    CHECK(gate.begin_snapshot(1, 2));
    CHECK(!gate.ready());
    CHECK(!gate.add_position(1, 1, identity(), "600000", 500, 500));
    CHECK(!gate.finish_positions(1, 2));
    CHECK(!gate.ready());
    CHECK(gate.begin_snapshot(1, 3));
    complete(gate, 1, 3);
}

void test_activity_requires_a_new_quiescent_snapshot() {
    AccountReconciliation gate = make_gate();
    start(gate);
    rows(gate);
    CHECK(!gate.on_activity(1));
    CHECK(!gate.finish_positions(1, 1));
    CHECK(!gate.finish_snapshot(1, 1));
    CHECK(gate.failure_reason().find("activity") != std::string::npos);
    CHECK(gate.begin_snapshot(1, 2));
    complete(gate, 1, 2);
    CHECK(gate.on_activity(1));
    CHECK(gate.ready());
}

void test_open_orders_or_unknown_completion_keep_gate_closed() {
    for (std::size_t count : {std::size_t(1), std::numeric_limits<std::size_t>::max()}) {
        AccountReconciliation gate = make_gate();
        start(gate);
        rows(gate);
        CHECK(gate.finish_positions(1, 1));
        CHECK(!gate.finish_orders(1, 1, count));
        CHECK(!gate.finish_orders(1, 1, 0));
        CHECK(!gate.finish_snapshot(1, 1));
        CHECK(!gate.ready());
    }
    AccountReconciliation gate = make_gate();
    start(gate);
    rows(gate);
    CHECK(gate.finish_positions(1, 1));
    CHECK(!gate.finish_snapshot(1, 1));
    CHECK(!gate.ready());
}

void test_disconnect_requires_a_fresh_snapshot_and_stop_is_permanent() {
    AccountReconciliation gate = make_gate();
    start(gate);
    complete(gate);
    CHECK(gate.mark_connected(1, false));
    CHECK(!gate.ready());
    CHECK_THROWS(std::logic_error, gate.positions());
    CHECK(gate.mark_connected(1, true));
    CHECK(!gate.ready());
    CHECK(!gate.finish_snapshot(1, 1));
    CHECK(gate.begin_snapshot(1, 2));
    complete(gate, 1, 2);
    gate.begin_stop();
    gate.begin_stop();
    CHECK(!gate.ready());
    CHECK(!gate.begin_epoch(2));
    CHECK(!gate.mark_connected(1, true));
    CHECK(!gate.begin_snapshot(1, 3));
    CHECK(!gate.add_account(1, 2, identity(), 1234.5));
    CHECK(!gate.on_activity(1));
    CHECK(gate.failure_reason() == "reconciliation stopped");
}

}  // namespace

int main() {
    try {
        test_initial_gate_closed_and_invalid_construction();
        test_all_queries_and_final_marker_required();
        test_independent_query_streams_may_arrive_in_any_order();
        test_missing_positions_or_completions_never_imply_success();
        test_account_identity_mismatch_invalidates_only_current_snapshot();
        test_duplicate_rows_and_completion_markers_fail_closed();
        test_invalid_cash_positions_and_unknown_instruments_are_rejected();
        test_stale_epoch_and_token_cannot_pollute_current_snapshot();
        test_epoch_and_token_must_strictly_increase();
        test_new_snapshot_requires_new_rows_and_ignores_old_token();
        test_activity_requires_a_new_quiescent_snapshot();
        test_open_orders_or_unknown_completion_keep_gate_closed();
        test_disconnect_requires_a_fresh_snapshot_and_stop_is_permanent();
        std::cout << "account_reconciliation_test: 13 cases PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "account_reconciliation_test: " << error.what() << '\n';
        return 1;
    }
}
