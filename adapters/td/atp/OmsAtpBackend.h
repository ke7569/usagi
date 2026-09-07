#ifndef USAGI_ATP_OMS_BACKEND_H
#define USAGI_ATP_OMS_BACKEND_H

#include "common/oms/Backend.h"

#include <map>
#include <string>

namespace oms {

// Collects the normalized rows produced by ATP account queries.  This helper
// deliberately does not certify all-day coverage; the caller supplies those
// protocol flags when taking the assembled snapshot.
class AtpSnapshotAssembler {
public:
    AtpSnapshotAssembler(const Scope& scope, std::uint64_t token)
        : scope_(scope), token_(token), free_cash_(0), free_cash_set_(false), valid_(true) {}

    bool set_free_cash(Money value) {
        if (value < 0) {
            valid_ = false;
            return false;
        }
        if (free_cash_set_ && free_cash_ != value) {
            valid_ = false;
            return false;
        }
        free_cash_ = value;
        free_cash_set_ = true;
        return true;
    }

    bool add_position(const SnapshotPosition& value) {
        if (value.instrument.market.empty() || value.instrument.code.empty() ||
            value.total < 0 || value.free_sellable < 0 || value.free_sellable > value.total) {
            valid_ = false;
            return false;
        }
        const auto inserted = positions_.insert(std::make_pair(value.instrument, value));
        if (inserted.second) {
            return true;
        }
        if (inserted.first->second.total == value.total &&
            inserted.first->second.free_sellable == value.free_sellable) {
            return true;
        }
        valid_ = false;
        return false;
    }

    bool add_order(const SnapshotOrder& value) {
        if (value.broker_id.empty() || value.instrument.market.empty() || value.instrument.code.empty() ||
            value.price <= 0 || value.original <= 0 || value.filled < 0 || value.working < 0 ||
            value.filled > value.original || value.working > value.original - value.filled ||
            (value.side != Side::Buy && value.side != Side::Sell)) {
            valid_ = false;
            return false;
        }
        const auto inserted = orders_.insert(std::make_pair(value.broker_id, value));
        if (inserted.second) {
            return true;
        }
        const SnapshotOrder& previous = inserted.first->second;
        if (previous.id == value.id && previous.owner == value.owner &&
            previous.instrument == value.instrument && previous.side == value.side &&
            previous.price == value.price && previous.original == value.original &&
            previous.filled == value.filled && previous.working == value.working &&
            previous.state == value.state) {
            return true;
        }
        valid_ = false;
        return false;
    }

    bool add_trade(const Report& value) {
        if (!(value.scope == scope_) || value.kind != ReportKind::Trade ||
            value.broker_id.empty() || value.trade_id.empty() || value.trade_quantity <= 0 ||
            value.trade_price < 0 || value.trade_fee < -1 ||
            (value.side != Side::Buy && value.side != Side::Sell)) {
            valid_ = false;
            return false;
        }
        const auto key = std::make_pair(value.broker_id, value.trade_id);
        const auto inserted = trades_.insert(std::make_pair(key, value));
        if (inserted.second) {
            return true;
        }
        const Report& previous = inserted.first->second;
        if (previous.scope == value.scope && previous.id == value.id &&
            previous.broker_id == value.broker_id && previous.instrument == value.instrument &&
            previous.side == value.side && previous.trade_quantity == value.trade_quantity &&
            previous.trade_price == value.trade_price && previous.trade_fee == value.trade_fee &&
            previous.cumulative_after == value.cumulative_after) {
            return true;
        }
        valid_ = false;
        return false;
    }

    bool valid() const { return valid_; }

    Snapshot snapshot(bool account_success, bool positions_success,
                      bool orders_success, bool trades_success,
                      bool all_day_orders, bool all_day_trades) const {
        Snapshot result;
        result.scope = scope_;
        result.token = token_;
        result.free_cash = free_cash_;
        result.account_success = account_success && free_cash_set_ && valid_;
        result.positions_success = positions_success && valid_;
        result.orders_success = orders_success && valid_;
        result.trades_success = trades_success && valid_;
        result.all_day_orders = all_day_orders;
        result.all_day_trades = all_day_trades;
        for (const auto& item : positions_) result.positions.push_back(item.second);
        for (const auto& item : orders_) result.orders.push_back(item.second);
        for (const auto& item : trades_) result.trades.push_back(item.second);
        return result;
    }

private:
    Scope scope_;
    std::uint64_t token_;
    Money free_cash_;
    bool free_cash_set_;
    bool valid_;
    std::map<Instrument, SnapshotPosition> positions_;
    std::map<std::string, SnapshotOrder> orders_;
    std::map<std::pair<std::string, std::string>, Report> trades_;
};

// Bound to one account and one immutable SDK connection generation.
// Query certification is deliberately not inferred from the presence of SDK APIs.
class AtpBackend : public Backend {
public:
    typedef std::function<SendResult(const Command&)> Sender;
    typedef std::function<Error(const Scope&, std::uint64_t)> Queryer;
    AtpBackend(const Scope& scope, const Sender& sender, const Queryer& queryer = Queryer())
        : scope_(scope), sender_(sender), queryer_(queryer) {}
    Capabilities capabilities() const override {
        Capabilities c; c.fills = FillCoverage::Cumulative;
        // ATP can rebuild the current risk state from fund/share/order
        // queries.  Historical trade coverage is retained as enrichment until
        // the broker's all-day semantics are certified, so it must not hold
        // account admission closed after a process restart.
        c.query_reconcile = true;
        c.complete_snapshot = false;
        c.trades_required_for_snapshot = false;
        return c;
    }
    SendResult submit(const Command& command) override { return send(command, false); }
    SendResult cancel(const Command& command) override { return send(command, true); }
    Error query(const Scope& scope, std::uint64_t token) override {
        std::lock_guard<std::mutex> guard(transport_mutex_);
        if (!queryer_ || !(scope == scope_) || !token) {
            Error e; e.category = ErrorCategory::Unsupported;
            e.message = "ATP query dispatch unavailable for this scope or generation";
            return e;
        }
        return queryer_(scope, token);
    }
    void publish(const Report& report) { if (report.scope == scope_) emit(report); }
    void publish(const Snapshot& snapshot) { if (snapshot.scope == scope_) emit(snapshot); }
    void connected() { connection(scope_, true); }
    void disconnected() { connection(scope_, false); }
    void close() {
        { std::lock_guard<std::mutex> guard(transport_mutex_); sender_ = Sender(); queryer_ = Queryer(); }
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
    Queryer queryer_;
    std::mutex transport_mutex_;
};

}  // namespace oms
#endif
