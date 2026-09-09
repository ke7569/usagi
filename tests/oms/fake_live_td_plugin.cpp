#include "common/execution/LiveTd.h"
#include "adapters/td/atp/OmsAtpBackend.h"
#include <cstdio>
#include <cstring>

namespace {
std::string trace_path;
void trace(const char* event) {
    FILE* out = std::fopen(trace_path.c_str(), "a");
    if (out) { std::fprintf(out, "%s\n", event); std::fclose(out); }
}
struct UnloadTrace { ~UnloadTrace() { trace("unload"); } } unload_trace;

// Explicit test simulation: use the real ATP callback queue without acquiring
// a real account lease or loading a broker SDK.
class FakeBackend : public oms::AtpBackend {
public:
    FakeBackend(const oms::Scope& scope, const Sender& sender, const QuerySender& query)
        : oms::AtpBackend(scope, sender, query, true, true) {}
    oms::Capabilities capabilities() const override {
        oms::Capabilities result = oms::AtpBackend::capabilities();
        result.simulated = true; return result;
    }
};
class FakeSession : public strategy_runtime::LiveTdSession {
public:
    FakeSession(const oms::Scope& scope, const std::set<oms::Instrument>& universe, bool allow)
        : scope_(scope), universe_(universe), allow_(allow), connected_(false), stopped_(false),
          submits_(0), cancels_(0), queries_(0) {
        backend_.reset(new FakeBackend(scope,
            [this](const oms::Command& command) { return send(command); },
            [this](const oms::Scope& scope, std::uint64_t token) {
                oms::Snapshot snapshot; snapshot.scope = scope; snapshot.token = token;
                snapshot.free_cash = 10000000000LL;
                snapshot.account_success = snapshot.positions_success = true;
                snapshot.orders_success = snapshot.trades_success = true;
                snapshot.all_day_orders = snapshot.all_day_trades = true;
                for (const auto& instrument : universe_) {
                    oms::SnapshotPosition p; p.instrument = instrument;
                    snapshot.positions.push_back(p);
                }
                ++queries_; backend_->publish_snapshot(snapshot); return oms::Error();
            }));
        trace(allow_ ? "create-orders" : "create-query-only");
    }
    ~FakeSession() override { stop(); trace("destroy"); }
    std::shared_ptr<oms::Backend> backend() const override { return backend_; }
    void attach(const std::shared_ptr<oms::Engine>& engine) override { engine_ = engine; }
    bool connect(long, std::string* error) override {
        if (error) error->clear();
        connected_ = true; backend_->connected(); trace("connect"); return true;
    }
    void stop() override {
        if (stopped_) return;
        stopped_ = true; connected_ = false; backend_->close(); trace("stop");
    }
    std::string status() const override {
        return std::string(connected_ ? "connected" : "stopped") +
            " submits=" + std::to_string(submits_) + " cancels=" + std::to_string(cancels_) +
            " queries=" + std::to_string(queries_);
    }
private:
    oms::SendResult send(const oms::Command& command) {
        oms::SendResult result;
        if (!allow_) {
            result.error.category = oms::ErrorCategory::Unsupported;
            result.error.message = "query-only session"; return result;
        }
        if (command.cancel) ++cancels_; else ++submits_;
        result.disposition = oms::SendDisposition::Submitted;
        result.broker_id = "FAKE-" + std::to_string(command.id);
        oms::Report report; report.scope = scope_; report.id = command.id;
        report.broker_id = result.broker_id; report.instrument = command.intent.instrument;
        report.side = command.intent.side; report.original = command.intent.quantity;
        report.kind = oms::ReportKind::Order; report.cumulative = 0;
        report.state = command.cancel ? oms::OrderState::Canceled : oms::OrderState::Accepted;
        report.leaves = command.cancel ? 0 : command.intent.quantity;
        backend_->publish(report); trace(command.cancel ? "cancel" : "submit");
        return result;
    }
    oms::Scope scope_;
    std::set<oms::Instrument> universe_;
    bool allow_, connected_, stopped_;
    unsigned submits_, cancels_, queries_;
    std::weak_ptr<oms::Engine> engine_;
    std::shared_ptr<FakeBackend> backend_;
};
}

#ifndef USAGI_TEST_MISSING_TD_ENTRYPOINT
extern "C" strategy_runtime::LiveTdSession* usagi_create_live_td_v1(
    const char* config, const oms::Scope* scope, const std::set<oms::Instrument>* universe,
    bool allow, char* error, std::size_t error_size) {
    trace_path = config ? config : "";
    if (!scope || !scope->epoch || !universe || trace_path.find("reject-after-gates") != std::string::npos) {
        trace("factory-reached");
        if (error && error_size) {
            std::strncpy(error, "fake TD factory reached", error_size - 1);
            error[error_size - 1] = 0;
        }
        return 0;
    }
    return new FakeSession(*scope, *universe, allow);
}
extern "C" void usagi_destroy_live_td_v1(strategy_runtime::LiveTdSession* session) { delete session; }
#endif
