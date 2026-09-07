#include "common/oms/Oms.h"
#include "common/oms/Journal.h"
#include "common/oms/Records.h"
#include "third_party/nlohmann/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>

namespace {
using namespace oms;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

const Instrument kSze = {"SZE", "000001"};
const Instrument kSse = {"SSE", "600000"};

struct TempDir {
    std::string path;
    std::string journal;
    TempDir() {
        char value[] = "/tmp/usagi-oms-recovery-XXXXXX";
        char* result = ::mkdtemp(value);
        require(result != 0, "mkdtemp");
        path = result;
        journal = path + "/account.journal";
    }
    ~TempDir() {
        const std::string lock = path + "/oms-7061706572-756e6974.lock";
        ::unlink(journal.c_str());
        ::unlink(lock.c_str());
        ::rmdir(path.c_str());
    }
};

InstrumentRules rules() {
    InstrumentRules value;
    value.tick = 100;
    value.lot = 100;
    value.lower_price = 90000;
    value.upper_price = 110000;
    return value;
}

Capabilities capabilities() {
    Capabilities value;
    value.simulated = true;
    value.complete_snapshot = true;
    value.fills = FillCoverage::DualFromOrigin;
    value.trades_required_for_snapshot = true;
    return value;
}

Config config(const TempDir& temp) {
    Config value;
    value.scope.account.broker = "paper";
    value.scope.account.account = "unit";
    value.scope.gateway = "fixture";
    value.scope.day = 20260904;
    value.scope.source = 1;
    value.instance = "recovery-test";
    value.enabled = true;
    value.ownership = OwnershipMode::ExclusiveLocal;
    value.journal_path = temp.journal;
    value.lock_directory = temp.path;
    value.instruments[kSze] = rules();
    value.instruments[kSse] = rules();
    return value;
}

Snapshot snapshot_for(const std::shared_ptr<Engine>& engine, std::uint64_t token) {
    Snapshot value;
    value.scope = engine->scope();
    value.token = token;
    value.free_cash = 1000000000000LL;
    value.account_success = true;
    value.positions_success = true;
    value.orders_success = true;
    value.trades_success = true;
    value.all_day_orders = true;
    value.all_day_trades = true;
    SnapshotPosition sze;
    sze.instrument = kSze; sze.total = 10000; sze.free_sellable = 10000;
    value.positions.push_back(sze);
    SnapshotPosition sse;
    sse.instrument = kSse; sse.total = 10000; sse.free_sellable = 10000;
    value.positions.push_back(sse);
    return value;
}

void reconcile(const std::shared_ptr<Engine>& engine,
               const std::shared_ptr<ScriptedBackend>& backend,
               std::uint64_t token) {
    require(engine->begin_reconcile(token, false), "begin reconcile");
    Snapshot value = snapshot_for(engine, token);
    backend->publish(value);
}

Intent buy(const std::string& owner, const std::string& id, Quantity quantity = 500) {
    Intent value;
    value.owner = owner;
    value.intent_id = id;
    value.signal_id = id + "-signal";
    value.instrument = kSze;
    value.side = Side::Buy;
    value.type = OrderType::Limit;
    value.price = 100000;
    value.quantity = quantity;
    return value;
}

Report order_report(const std::shared_ptr<Engine>& engine, OrderId id,
                    const Instrument& instrument, Side side,
                    OrderState state, Quantity filled, Quantity working,
                    const std::string& broker) {
    Report value;
    value.scope = engine->scope();
    value.id = id;
    value.broker_id = broker;
    value.instrument = instrument;
    value.side = side;
    value.kind = ReportKind::Order;
    value.state = state;
    value.cumulative = filled;
    value.leaves = working;
    return value;
}

SnapshotOrder snapshot_order(OrderId id, const std::string& owner,
                             const std::string& broker, const Instrument& instrument,
                             Side side, Quantity original, Quantity filled,
                             Quantity working, OrderState state) {
    SnapshotOrder value;
    value.id = id; value.owner = owner; value.broker_id = broker;
    value.instrument = instrument; value.side = side; value.price = 100000;
    value.original = original; value.filled = filled; value.working = working;
    value.state = state;
    return value;
}

std::shared_ptr<Engine> create(const Config& value,
                               const std::shared_ptr<ScriptedBackend>& backend,
                               std::uint64_t epoch) {
    std::shared_ptr<Engine> engine = Engine::create(value, backend);
    require(engine->start_epoch(epoch), "start recovered epoch");
    return engine;
}

void test_durable_active_order_recovery() {
    TempDir temp;
    Config value = config(temp);
    std::shared_ptr<ScriptedBackend> first_backend(new ScriptedBackend(capabilities()));
    int first_submits = 0;
    first_backend->submit_hook = [&first_submits](const Command&) {
        ++first_submits;
        SendResult result; result.disposition = SendDisposition::Submitted; result.broker_id = "B1"; return result;
    };
    std::shared_ptr<Engine> first = create(value, first_backend, 1);
    reconcile(first, first_backend, 1);
    SubmitResult sent = first->submit(buy("owner", "durable"));
    require(sent.accepted && first_submits == 1, "durable order submitted once");
    OrderView before;
    require(first->order(sent.id, &before) && before.working == 500, "active order persisted in memory");
    first.reset();

    std::shared_ptr<ScriptedBackend> second_backend(new ScriptedBackend(capabilities()));
    int second_submits = 0;
    second_backend->submit_hook = [&second_submits](const Command&) {
        ++second_submits; SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    std::shared_ptr<Engine> second = create(value, second_backend, 2);
    OrderView recovered;
    require(second->order(sent.id, &recovered) && recovered.state == OrderState::Unknown,
            "active order recovered as unknown before reconciliation");
    Snapshot value_snapshot = snapshot_for(second, 2);
    require(second->begin_reconcile(2, false), "begin active order recovery");
    value_snapshot.orders.push_back(snapshot_order(sent.id, "owner", "B1", kSze,
                                                    Side::Buy, 500, 0, 500,
                                                    OrderState::Accepted));
    second_backend->publish(value_snapshot);
    require(!second->account().ready, "recovery rate gate remains closed initially");
    require(second_submits == 0, "recovery never resubmits active order");
    second->advance_to(value.limits.rate_window_ns);
    require(second->account().ready, "recovery ready after controlled rate window");
    require(second->order(sent.id, &recovered) && recovered.working == 500,
            "recovered working reservation retained");
}

void test_unknown_send_is_not_replayed() {
    TempDir temp;
    Config value = config(temp);
    std::shared_ptr<ScriptedBackend> first_backend(new ScriptedBackend(capabilities()));
    first_backend->submit_hook = [](const Command&) {
        SendResult result; result.disposition = SendDisposition::Unknown; return result;
    };
    std::shared_ptr<Engine> first = create(value, first_backend, 1);
    reconcile(first, first_backend, 1);
    SubmitResult unknown = first->submit(buy("owner", "unknown"));
    require(unknown.accepted, "unknown send retained");
    first.reset();
    std::shared_ptr<ScriptedBackend> second_backend(new ScriptedBackend(capabilities()));
    int submits = 0;
    second_backend->submit_hook = [&submits](const Command&) {
        ++submits; SendResult result; result.disposition = SendDisposition::Submitted; return result;
    };
    std::shared_ptr<Engine> second = create(value, second_backend, 2);
    reconcile(second, second_backend, 2);
    second->advance_to(value.limits.rate_window_ns);
    require(submits == 0, "unknown send is never blindly replayed");
    require(!second->account().ready, "unknown send keeps account closed");
}

void test_external_order_is_read_only_and_self_crosses() {
    TempDir temp;
    Config value = config(temp);
    std::shared_ptr<ScriptedBackend> backend(new ScriptedBackend(capabilities()));
    std::shared_ptr<Engine> engine = create(value, backend, 1);
    reconcile(engine, backend, 1);
    Snapshot value_snapshot = snapshot_for(engine, 2);
    value_snapshot.positions[0].free_sellable -= 500;
    value_snapshot.orders.push_back(snapshot_order(77, "foreign", "EXT77", kSze,
                                                   Side::Sell, 500, 0, 500,
                                                   OrderState::Accepted));
    require(engine->begin_reconcile(2, false), "external reconcile");
    backend->publish(value_snapshot);
    engine->advance_to(value.limits.rate_window_ns);
    SubmitResult crossing = engine->submit(buy("owner", "cross", 100));
    require(!crossing.accepted && crossing.error.category == ErrorCategory::SelfTrade,
            "external order participates in self-cross protection");
    require(!engine->owns("owner", (1ULL << 63), kSze), "external order is not owned");
}

void test_incomplete_query_and_overlapping_report_close_gate() {
    TempDir temp;
    Config value = config(temp);
    std::shared_ptr<ScriptedBackend> backend(new ScriptedBackend(capabilities()));
    std::shared_ptr<Engine> engine = create(value, backend, 1);
    require(engine->begin_reconcile(1, false), "incomplete query start");
    Snapshot incomplete = snapshot_for(engine, 1);
    incomplete.trades_success = false;
    backend->publish(incomplete);
    require(!engine->account().ready, "incomplete query closes gate");

    require(engine->begin_reconcile(2, false), "overlap query start");
    Report report = order_report(engine, 123, kSze, Side::Buy, OrderState::Accepted, 0, 100, "ORPHAN");
    engine->report(report);
    Snapshot overlapping = snapshot_for(engine, 2);
    backend->publish(overlapping);
    require(!engine->account().ready, "query/report overlap closes gate");
}

void test_journal_failure_and_corrupt_tail_close_gate() {
    TempDir corrupt;
    Config value = config(corrupt);
    std::shared_ptr<ScriptedBackend> backend(new ScriptedBackend(capabilities()));
    std::shared_ptr<Engine> engine = create(value, backend, 1);
    reconcile(engine, backend, 1);
    engine.reset();
    int fd = ::open(corrupt.journal.c_str(), O_RDWR | O_CLOEXEC);
    require(fd >= 0, "open journal for corruption");
    require(::pwrite(fd, "x", 1, 20) == 1, "corrupt journal tail");
    ::close(fd);
    std::shared_ptr<ScriptedBackend> corrupt_backend(new ScriptedBackend(capabilities()));
    std::shared_ptr<Engine> corrupt_engine = create(value, corrupt_backend, 2);
    require(!corrupt_engine->account().ready && !corrupt_engine->account().durable,
            "corrupt replay closes durable gate");

    TempDir failed;
    Config failed_config = config(failed);
    failed_config.journal_path = failed.path + "/missing/account.journal";
    std::shared_ptr<ScriptedBackend> failed_backend(new ScriptedBackend(capabilities()));
    std::shared_ptr<Engine> failed_engine = create(failed_config, failed_backend, 1);
    require(!failed_engine->account().ready && !failed_engine->account().durable,
            "journal open failure closes gate");
}

void test_crash_after_intent_never_proves_unsent() {
    TempDir temp;
    const Config settings = config(temp);
    const pid_t child = ::fork();
    require(child >= 0, "crash fixture fork");
    if (child == 0) {
        try {
            std::shared_ptr<ScriptedBackend> backend(new ScriptedBackend(capabilities()));
            auto engine = create(settings, backend, 1);
            reconcile(engine, backend, 1);
            backend->submit_hook = [](const Command&) -> SendResult { ::_exit(0); };
            engine->submit(buy("owner", "crash-after-intent"));
        } catch (...) { ::_exit(3); }
        ::_exit(4);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "child crashed inside backend after durable registration");
    TempDir prefix;
    {
        Journal original(temp.journal), durable_prefix(prefix.journal);
        require(durable_prefix.replay(0), "new prefix journal");
        require(original.replay([&](const std::string& payload) {
            const auto row = oms::records::decode(payload);
            if (row.at("type").get<std::string>() == "dispatch") return true;
            return durable_prefix.append(payload, true);
        }), "simulate loss of complete non-durable dispatch frame");
    }
    Config recovered_config = settings;
    recovered_config.journal_path = prefix.journal; recovered_config.lock_directory = prefix.path;
    std::shared_ptr<ScriptedBackend> backend(new ScriptedBackend(capabilities()));
    unsigned sends = 0;
    backend->submit_hook = [&sends](const Command&) { ++sends; return SendResult(); };
    auto engine = create(recovered_config, backend, 2);
    reconcile(engine, backend, 2); engine->advance_to(settings.limits.rate_window_ns);
    OrderView order;
    require(engine->order(1, &order) && order.working == 500 && order.state == OrderState::Unknown,
            "intent without send result is still possibly sent after crash");
    require(!engine->account().ready && sends == 0, "missing dispatch/result never authorizes blind resend or release");
}

void test_live_journal_failure_blocks_new_risk_but_allows_owned_cancel() {
    TempDir temp;
    const Config settings = config(temp);
    const pid_t child = ::fork();
    require(child >= 0, "journal write failure fork");
    if (child == 0) {
        try {
            std::shared_ptr<ScriptedBackend> backend(new ScriptedBackend(capabilities()));
            auto engine = create(settings, backend, 1);
            reconcile(engine, backend, 1);
            const auto active = engine->submit(buy("owner", "before-disk-failure"));
            require(active.accepted, "active before disk failure");
            struct stat info;
            require(::stat(temp.journal.c_str(), &info) == 0, "journal size before failure");
            struct rlimit limit;
            require(::getrlimit(RLIMIT_FSIZE, &limit) == 0, "read file-size limit");
            limit.rlim_cur = static_cast<rlim_t>(info.st_size + 8);
            ::signal(SIGXFSZ, SIG_IGN);
            require(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "child-only partial write failure");
            unsigned sends = 0, cancels = 0;
            backend->submit_hook = [&sends](const Command&) { ++sends; return SendResult(); };
            backend->cancel_hook = [&cancels](const Command&) {
                ++cancels; SendResult r; r.disposition = SendDisposition::Submitted; return r;
            };
            const auto denied = engine->submit(buy("owner", "after-disk-failure"));
            require(!denied.accepted && sends == 0 && !engine->account().ready,
                    "failed durable preregistration never reaches TD");
            require(!engine->cancel("owner", active.id).failed() && cancels == 1,
                    "known risk-reducing cancel survives journal failure");
            OrderView view;
            require(engine->order(active.id, &view) && view.working == 500, "cancel intent does not release reservation");
        } catch (...) { ::_exit(3); }
        ::_exit(0);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "journal partial-write failure remains fail-closed with owned cancels");
}

}  // namespace

int main() {
    try {
        test_durable_active_order_recovery();
        test_unknown_send_is_not_replayed();
        test_external_order_is_read_only_and_self_crosses();
        test_incomplete_query_and_overlapping_report_close_gate();
        test_journal_failure_and_corrupt_tail_close_gate();
        test_crash_after_intent_never_proves_unsent();
        test_live_journal_failure_blocks_new_risk_but_allows_owned_cancel();
    } catch (const std::exception& error) {
        std::cerr << "oms_recovery_test: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
