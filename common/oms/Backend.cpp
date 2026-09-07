#include "common/oms/Backend.h"

namespace oms {

Capabilities PaperBackend::capabilities() const {
    Capabilities result;
    result.simulated = true;
    result.complete_snapshot = true;
    result.cancel_requires_broker_id = false;
    result.cancel_acknowledgements = false;
    result.trades_required_for_snapshot = false;
    result.fills = FillCoverage::Cumulative;
    return result;
}

SendResult PaperBackend::submit(const Command& command) {
    if (observer_) observer_(command);
    SendResult result;
    result.disposition = SendDisposition::Submitted;
    return result;
}

SendResult PaperBackend::cancel(const Command& command) {
    if (observer_) observer_(command);
    SendResult result;
    result.disposition = SendDisposition::Submitted;
    return result;
}

Error PaperBackend::query(const Scope&, std::uint64_t) {
    Error error;
    error.category = ErrorCategory::Unsupported;
    error.message = "paper requires an explicit configured simulation snapshot";
    return error;
}

ScriptedBackend::ScriptedBackend(const Capabilities& capabilities)
    : capabilities_(capabilities) {
    if (!capabilities_.simulated)
        throw std::invalid_argument("scripted backend must declare simulation");
}

SendResult ScriptedBackend::submit(const Command& command) {
    if (submit_hook) return submit_hook(command);
    SendResult result;
    result.disposition = SendDisposition::Submitted;
    result.broker_id = "scripted-" + std::to_string(command.id);
    return result;
}

SendResult ScriptedBackend::cancel(const Command& command) {
    if (cancel_hook) return cancel_hook(command);
    SendResult result;
    result.disposition = SendDisposition::Submitted;
    return result;
}

Error ScriptedBackend::query(const Scope& scope, std::uint64_t token) {
    if (query_hook) return query_hook(scope, token);
    Error error;
    error.category = ErrorCategory::Unsupported;
    error.message = "scripted snapshot hook not configured";
    return error;
}

}  // namespace oms
