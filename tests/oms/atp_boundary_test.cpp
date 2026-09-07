#include "adapters/td/atp/OmsAtpBackend.h"
#include "common/oms/Oms.h"

#include <cstdlib>
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

}  // namespace

int main() {
    try {
        test_real_capabilities_and_simulation_rejection();
        test_scope_cancel_and_native_fak_guards();
        test_sender_dispositions_raw_error_and_close();
        test_publish_scope_and_connection_callback();
        test_query_is_explicitly_unsupported();
    } catch (const std::exception& error) {
        std::cerr << "atp_boundary_test: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
