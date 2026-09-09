#ifndef USAGI_ATP_OMS_BACKEND_H
#define USAGI_ATP_OMS_BACKEND_H

#include "common/oms/Backend.h"
#include <atomic>
#include <deque>
#include <utility>

namespace oms {

// Bound to one account and one immutable SDK connection generation.
// Query certification is deliberately not inferred from the presence of SDK APIs.
class AtpBackend : public Backend {
public:
    typedef std::function<SendResult(const Command&)> Sender;
    typedef std::function<Error(const Scope&, std::uint64_t)> QuerySender;
    AtpBackend(const Scope& scope, const Sender& sender,
               const QuerySender& query = QuerySender(), bool certified_snapshot = false,
               bool defer_callbacks = false, std::size_t callback_capacity = 4096)
        : scope_(scope), sender_(sender), query_(query), certified_snapshot_(certified_snapshot),
          defer_callbacks_(defer_callbacks), callback_capacity_(callback_capacity ? callback_capacity : 1),
          transport_blocked_(defer_callbacks), closed_(false),
          disconnect_pending_(false), connection_broken_(false), callback_failed_(false) {}
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
        QuerySender query;
        {
            std::lock_guard<std::mutex> guard(transport_mutex_);
            if (!(scope == scope_) || !token || closed_.load() || transport_blocked_.load()) {
                Error e; e.category = ErrorCategory::NotReady;
                e.message = "ATP query transport closed or wrong generation"; return e;
            }
            query = query_;
        }
        if (query) return query(scope, token);
        Error e; e.category = ErrorCategory::Unsupported;
        e.message = "ATP recovery not certified: account cash semantics, full pagination and connection replay boundary required";
        return e;
    }
    void publish(const Report& report) {
        if (!(report.scope == scope_)) return;
        if (!defer_callbacks_) { emit(report); return; }
        try {
            Event event; event.kind = Event::ReportEvent; event.report = report;
            enqueue(std::move(event));
        } catch (...) { callback_failure(); }
    }
    // Broker-side query aggregation publishes the certified account snapshot;
    // the OMS Engine consumes it as complete_snapshot() through this sink.
    void publish_snapshot(const Snapshot& snapshot) {
        if (!(snapshot.scope == scope_)) return;
        if (!defer_callbacks_) { emit(snapshot); return; }
        try {
            Event event; event.kind = Event::SnapshotEvent; event.snapshot = snapshot;
            enqueue(std::move(event));
        } catch (...) { callback_failure(); }
    }
    void connected() {
        if (closed_.load()) return;
        if (!defer_callbacks_) {
            transport_blocked_.store(false); connection(scope_, true); return;
        }
        try {
            Event event; event.kind = Event::ConnectedEvent;
            enqueue(std::move(event));
        } catch (...) { callback_failure(); }
    }
    void disconnected() {
        transport_blocked_.store(true);
        if (!defer_callbacks_) { connection(scope_, false); return; }
        std::lock_guard<std::mutex> guard(callback_mutex_);
        transport_blocked_.store(true);
        connection_broken_ = true; disconnect_pending_ = true;
        // This backend represents one immutable SDK generation. A reconnect
        // requires a new backend/scope; stale query results cannot certify it.
        for (auto i = callbacks_.begin(); i != callbacks_.end();) {
            if (i->kind != Event::ReportEvent) i = callbacks_.erase(i);
            else ++i;
        }
    }
    // Only the owner thread drains callbacks. SDK threads never enter OMS.
    void advance_to(Time) override {
        if (!defer_callbacks_) return;
        for (std::size_t drained = 0; drained <= callback_capacity_; ++drained) {
            Event event; bool disconnect = false;
            {
                std::lock_guard<std::mutex> guard(callback_mutex_);
                if (disconnect_pending_) {
                    disconnect_pending_ = false; disconnect = true;
                } else if (!callbacks_.empty()) {
                    event = std::move(callbacks_.front()); callbacks_.pop_front();
                    if (connection_broken_ && event.kind != Event::ReportEvent) continue;
                    if (event.kind == Event::ConnectedEvent) transport_blocked_.store(false);
                } else break;
            }
            if (disconnect) connection(scope_, false);
            else if (event.kind == Event::ConnectedEvent) connection(scope_, true);
            else if (event.kind == Event::SnapshotEvent) emit(event.snapshot);
            else emit(event.report);
        }
    }
    void close() {
        closed_.store(true); transport_blocked_.store(true);
        { std::lock_guard<std::mutex> guard(transport_mutex_); sender_ = Sender(); query_ = QuerySender(); }
        disconnected();
    }
    Scope scope() const { return scope_; }
private:
    struct Event {
        enum Kind { ReportEvent, SnapshotEvent, ConnectedEvent } kind;
        Report report;
        Snapshot snapshot;
        Event() : kind(ReportEvent) {}
    };
    void enqueue(Event&& event) {
        std::lock_guard<std::mutex> guard(callback_mutex_);
        if (closed_.load() || callback_failed_ ||
            (connection_broken_ && event.kind != Event::ReportEvent)) return;
        if (callbacks_.size() >= callback_capacity_) {
            fail_callbacks_locked(); return;
        }
        callbacks_.push_back(std::move(event));
    }
    void fail_callbacks_locked() {
        transport_blocked_.store(true);
        callback_failed_ = connection_broken_ = disconnect_pending_ = true;
        callbacks_.clear();
    }
    void callback_failure() {
        std::lock_guard<std::mutex> guard(callback_mutex_);
        fail_callbacks_locked();
    }
    SendResult send(const Command& command, bool cancel) {
        std::lock_guard<std::mutex> guard(transport_mutex_);
        if (!sender_ || closed_.load() || transport_blocked_.load() ||
            !(command.scope == scope_) || command.cancel != cancel || !command.id ||
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
    const bool defer_callbacks_;
    const std::size_t callback_capacity_;
    std::atomic<bool> transport_blocked_, closed_;
    std::mutex callback_mutex_;
    std::deque<Event> callbacks_;
    bool disconnect_pending_, connection_broken_, callback_failed_;
};

}  // namespace oms
#endif
