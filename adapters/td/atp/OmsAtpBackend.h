#ifndef USAGI_ATP_OMS_BACKEND_H
#define USAGI_ATP_OMS_BACKEND_H

#include "common/oms/Backend.h"

namespace oms {

// Bound to one account and one immutable SDK connection generation.
// Query certification is deliberately not inferred from the presence of SDK APIs.
class AtpBackend : public Backend {
public:
    typedef std::function<SendResult(const Command&)> Sender;
    typedef std::function<Error(const Scope&, std::uint64_t)> QuerySender;
    AtpBackend(const Scope& scope, const Sender& sender,
               const QuerySender& query = QuerySender(), bool certified_snapshot = false)
        : scope_(scope), sender_(sender), query_(query), certified_snapshot_(certified_snapshot) {}
    Capabilities capabilities() const override {
        Capabilities c; c.fills = FillCoverage::Cumulative;
        // complete_snapshot is only claimed once a query sender is installed
        // and the broker snapshot assembly (fund/positions/orders/trades with
        // full pagination and connection replay boundary) is certified.
        c.complete_snapshot = certified_snapshot_;
        c.trades_required_for_snapshot = true; return c;
    }
    SendResult submit(const Command& command) override { return send(command, false); }
    SendResult cancel(const Command& command) override { return send(command, true); }
    Error query(const Scope& scope, std::uint64_t token) override {
        if (query_) return query_(scope, token);
        Error e; e.category = ErrorCategory::Unsupported;
        e.message = "ATP recovery not certified: account cash semantics, full pagination and connection replay boundary required";
        return e;
    }
    void publish(const Report& report) { if (report.scope == scope_) emit(report); }
    // Broker-side query aggregation publishes the certified account snapshot;
    // the OMS Engine consumes it as complete_snapshot() through this sink.
    void publish_snapshot(const Snapshot& snapshot) { if (snapshot.scope == scope_) emit(snapshot); }
    void disconnected() { connection(scope_, false); }
    void close() {
        { std::lock_guard<std::mutex> guard(transport_mutex_); sender_ = Sender(); }
        disconnected();
    }
    Scope scope() const { return scope_; }
private:
    SendResult send(const Command& command, bool cancel) {
        std::lock_guard<std::mutex> guard(transport_mutex_);
        if (!sender_ || !(command.scope == scope_) || command.cancel != cancel || !command.id ||
            (!cancel && command.intent.type == OrderType::NativeFak)) {
            SendResult r; r.error.category = ErrorCategory::Unsupported;
            r.error.message = "ATP transport closed, wrong generation or unsupported order type"; return r;
        }
        return sender_(command);
    }
    Scope scope_;
    Sender sender_;
    QuerySender query_;
    bool certified_snapshot_;
    std::mutex transport_mutex_;
};

}  // namespace oms
#endif
