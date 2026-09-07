#ifndef USAGI_OMS_BACKEND_H
#define USAGI_OMS_BACKEND_H

#include "common/oms/Types.h"
#include <mutex>
#include <stdexcept>

namespace oms {

class Backend {
public:
    typedef std::function<void(const Report&)> ReportSink;
    typedef std::function<void(const Snapshot&)> SnapshotSink;
    typedef std::function<void(const Scope&, bool)> ConnectionSink;
    virtual ~Backend() {}
    virtual Capabilities capabilities() const = 0;
    virtual SendResult submit(const Command& command) = 0;
    virtual SendResult cancel(const Command& command) = 0;
    virtual Error query(const Scope& scope, std::uint64_t token) = 0;
    virtual void advance_to(Time) {}

    void bind(const ReportSink& reports, const SnapshotSink& snapshots,
              const ConnectionSink& connections = ConnectionSink()) {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        if (reports_ || snapshots_) throw std::logic_error("backend already has an OMS owner");
        reports_ = reports; snapshots_ = snapshots; connections_ = connections;
    }
    void unbind() {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        reports_ = ReportSink(); snapshots_ = SnapshotSink(); connections_ = ConnectionSink();
    }
protected:
    void connection(const Scope& scope, bool connected) {
        ConnectionSink sink;
        { std::lock_guard<std::mutex> lock(sink_mutex_); sink = connections_; }
        if (sink) sink(scope, connected);
    }
    void emit(const Report& report) {
        ReportSink sink;
        { std::lock_guard<std::mutex> lock(sink_mutex_); sink = reports_; }
        if (sink) sink(report);
    }
    void emit(const Snapshot& snapshot) {
        SnapshotSink sink;
        { std::lock_guard<std::mutex> lock(sink_mutex_); sink = snapshots_; }
        if (sink) sink(snapshot);
    }
private:
    std::mutex sink_mutex_;
    ReportSink reports_;
    SnapshotSink snapshots_;
    ConnectionSink connections_;
};

// Command observation only. Neither submission nor cancellation creates reports.
class PaperBackend : public Backend {
public:
    explicit PaperBackend(const std::function<void(const Command&)>& observer)
        : observer_(observer) {}
    Capabilities capabilities() const override;
    SendResult submit(const Command& command) override;
    SendResult cancel(const Command& command) override;
    Error query(const Scope&, std::uint64_t) override;
private:
    std::function<void(const Command&)> observer_;
};

// Tests inject explicit reports, not a fill model. Hooks run outside OMS state locks.
class ScriptedBackend : public Backend {
public:
    explicit ScriptedBackend(const Capabilities& capabilities);
    Capabilities capabilities() const override { return capabilities_; }
    SendResult submit(const Command& command) override;
    SendResult cancel(const Command& command) override;
    Error query(const Scope& scope, std::uint64_t token) override;
    void publish(const Report& report) { emit(report); }
    void publish(const Snapshot& snapshot) { emit(snapshot); }
    std::function<SendResult(const Command&)> submit_hook;
    std::function<SendResult(const Command&)> cancel_hook;
    std::function<Error(const Scope&, std::uint64_t)> query_hook;
private:
    Capabilities capabilities_;
};

}  // namespace oms
#endif
