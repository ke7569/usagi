#include "adapters/td/atp/OmsAtpBackend.h"
#include "common/oms/Oms.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
    require(!caps.simulated && !caps.complete_snapshot && !caps.native_fak,
            "ATP backend must remain real, uncertified and non-native-FAK");
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
    int disconnected = 0;
    backend.bind([&reports](const Report&) { ++reports; },
                 Backend::SnapshotSink(),
                 [&disconnected](const Scope& value, bool connected) {
                     require(value == scope(), "connection scope changed");
                     if (!connected) ++disconnected;
                 });
    Report current;
    current.scope = expected;
    current.id = 17;
    Report foreign = current;
    foreign.scope.epoch++;
    backend.publish(foreign);
    backend.publish(current);
    require(reports == 1, "only current scope report published");
    backend.disconnected();
    require(disconnected == 1, "disconnect callback reports false");
    backend.unbind();
}

void test_query_is_explicitly_unsupported() {
    AtpBackend backend(scope(), [](const Command&) { return SendResult(); });
    const Error error = backend.query(scope(), 1);
    require(error.category == ErrorCategory::Unsupported && !error.message.empty(),
            "ATP query certification remains explicitly unsupported");
}

void test_deferred_callbacks_run_on_owner_thread() {
    const std::thread::id owner = std::this_thread::get_id();
    std::vector<std::string> delivered;
    AtpBackend backend(scope(), [](const Command&) {
        SendResult r; r.disposition = SendDisposition::Submitted; return r;
    }, AtpBackend::QuerySender(), true, true);
    backend.bind([&](const Report&) {
        require(std::this_thread::get_id() == owner, "report entered OMS from SDK thread");
        delivered.push_back("report");
    }, [&](const Snapshot&) {
        require(std::this_thread::get_id() == owner, "snapshot entered OMS from SDK thread");
        delivered.push_back("snapshot");
    }, [&](const Scope&, bool connected) {
        require(std::this_thread::get_id() == owner, "connection entered OMS from SDK thread");
        delivered.push_back(connected ? "connected" : "disconnected");
    });
    require(backend.submit(command(scope())).error.failed(), "unconnected deferred transport must block sends");
    std::thread sdk([&]() {
        backend.connected();
        Report r; r.scope = scope(); backend.publish(r);
        Snapshot s; s.scope = scope(); backend.publish_snapshot(s);
    });
    sdk.join();
    require(delivered.empty(), "SDK callbacks must wait for owner advance");
    backend.advance_to(1);
    require(delivered.size() == 3 && delivered[0] == "connected" &&
            delivered[1] == "report" && delivered[2] == "snapshot", "callback order changed");
    require(!backend.submit(command(scope())).error.failed(), "owner connection acknowledgement permits send");
}

void test_deferred_disconnect_discards_old_certification() {
    std::vector<std::string> delivered;
    int sends = 0;
    AtpBackend backend(scope(), [&](const Command&) { ++sends; return SendResult(); },
                       AtpBackend::QuerySender(), true, true);
    backend.bind([&](const Report&) { delivered.push_back("report"); },
        [&](const Snapshot&) { delivered.push_back("snapshot"); },
        [&](const Scope&, bool value) { delivered.push_back(value ? "connected" : "disconnected"); });
    backend.connected(); backend.advance_to(1); delivered.clear();
    Snapshot snapshot; snapshot.scope = scope(); snapshot.token = 5;
    backend.publish_snapshot(snapshot);
    Report report; report.scope = scope(); backend.publish(report);
    backend.disconnected();
    // Late SDK login/query callbacks belong to the broken immutable generation.
    backend.connected(); backend.publish_snapshot(snapshot);
    require(backend.submit(command(scope())).error.failed() && sends == 0,
            "disconnect must block transport before owner drains callbacks");
    backend.advance_to(2);
    require(delivered.size() == 2 && delivered[0] == "disconnected" && delivered[1] == "report",
            "disconnect must precede reports and discard queued certification");
    require(backend.submit(command(scope())).error.failed(), "stale connected event reopened transport");
}

void test_deferred_overflow_and_close_block_sender() {
    int reports = 0, snapshots = 0, disconnected = 0, sends = 0;
    AtpBackend backend(scope(), [&](const Command&) { ++sends; return SendResult(); },
                       AtpBackend::QuerySender(), true, true, 2);
    backend.bind([&](const Report&) { ++reports; }, [&](const Snapshot&) { ++snapshots; },
        [&](const Scope&, bool value) { if (!value) ++disconnected; });
    backend.connected(); backend.advance_to(1);
    Report report; report.scope = scope();
    std::thread sdk([&]() { backend.publish(report); backend.publish(report); backend.publish(report); });
    sdk.join();
    backend.connected();
    Snapshot snapshot; snapshot.scope = scope(); backend.publish_snapshot(snapshot);
    require(backend.submit(command(scope())).error.failed() && sends == 0, "queue overflow did not block sender");
    backend.advance_to(2);
    require(disconnected == 1 && reports == 0 && snapshots == 0, "overflow must disconnect without partial certification");
    backend.close(); backend.connected();
    require(backend.submit(command(scope())).error.failed(), "close must immediately block sender");
    backend.advance_to(3);
    require(disconnected == 2, "close connection event must drain on owner");
}

}  // namespace

int main() {
    try {
        test_real_capabilities_and_simulation_rejection();
        test_scope_cancel_and_native_fak_guards();
        test_sender_dispositions_raw_error_and_close();
        test_publish_scope_and_connection_callback();
        test_query_is_explicitly_unsupported();
        test_deferred_callbacks_run_on_owner_thread();
        test_deferred_disconnect_discards_old_certification();
        test_deferred_overflow_and_close_block_sender();
    } catch (const std::exception& error) {
        std::cerr << "atp_boundary_test: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
