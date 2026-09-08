#include "common/oms/Oms.h"
#include "common/oms/Journal.h"
#include "common/oms/AsyncJournal.h"
#include "common/oms/Records.h"
#include "common/oms/Profile.h"
#include "common/execution/AccountReconciliation.h"
#include "third_party/nlohmann/json.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>

namespace oms {
namespace {
typedef nlohmann::json Json;

Error error(ErrorCategory category, const std::string& message) {
    Error result; result.category = category; result.message = message; return result;
}
bool text_ok(const std::string& value, std::size_t maximum = 128) {
    if (value.empty() || value.size() > maximum) return false;
    for (unsigned char c : value) if (c < 32 || c > 126) return false;
    return true;
}
bool instrument_ok(const Instrument& value) {
    if ((value.market != "SZE" && value.market != "SSE") || value.code.size() != 6) return false;
    return value.code.find_first_not_of("0123456789") == std::string::npos;
}
Money product(Money a, Quantity b) {
    if (a < 0 || b < 0 || (b && a > std::numeric_limits<Money>::max() / b))
        throw std::overflow_error("OMS notional overflow");
    return a * b;
}
Money sum(Money a, Money b) {
    if ((b > 0 && a > std::numeric_limits<Money>::max() - b) ||
        (b < 0 && a < std::numeric_limits<Money>::min() - b))
        throw std::overflow_error("OMS account arithmetic overflow");
    return a + b;
}
Money difference(Money a, Money b) {
    if ((b > 0 && a < std::numeric_limits<Money>::min() + b) ||
        (b < 0 && a > std::numeric_limits<Money>::max() + b))
        throw std::overflow_error("OMS account subtraction overflow");
    return a - b;
}
Time later(Time now, Time delay) {
    if (now < 0 || delay < 0 || now > std::numeric_limits<Time>::max() - delay)
        throw std::overflow_error("OMS deadline overflow");
    return now + delay;
}
bool gate_allows(const std::function<bool()>& gate) {
    try { return !gate || gate(); } catch (...) { return false; }
}
bool terminal(OrderState state) {
    return state == OrderState::Canceled || state == OrderState::Rejected || state == OrderState::Filled;
}
Json encode(const Instrument& value) { return Json{{"market", value.market}, {"code", value.code}}; }
Instrument decode_instrument(const Json& value) {
    Instrument result; result.market = value.at("market").get<std::string>();
    result.code = value.at("code").get<std::string>(); return result;
}
Json encode(const Scope& value) {
    return Json{{"broker", value.account.broker}, {"account", value.account.account},
        {"gateway", value.gateway}, {"day", value.day}, {"epoch", value.epoch}, {"source", value.source}};
}
Scope decode_scope(const Json& value) {
    Scope result; result.account.broker = value.at("broker").get<std::string>();
    result.account.account = value.at("account").get<std::string>();
    result.gateway = value.at("gateway").get<std::string>(); result.day = value.at("day").get<std::uint32_t>();
    result.epoch = value.at("epoch").get<std::uint64_t>(); result.source = value.at("source").get<short>();
    return result;
}
Json encode(const Error& value) {
    return Json{{"category", static_cast<int>(value.category)}, {"raw_code", value.raw_code},
        {"message", value.message.substr(0, 256)}, {"raw_type", value.raw_type.substr(0, 64)}};
}
Error decode_error(const Json& value) {
    Error result; result.category = static_cast<ErrorCategory>(value.at("category").get<int>());
    result.raw_code = value.at("raw_code").get<int>(); result.message = value.at("message").get<std::string>();
    result.raw_type = value.at("raw_type").get<std::string>(); return result;
}
Intent decode_intent(const Json& value) {
    Intent result; result.owner = value.at("owner").get<std::string>();
    result.intent_id = value.at("intent_id").get<std::string>(); result.signal_id = value.at("signal_id").get<std::string>();
    result.instrument = decode_instrument(value.at("instrument")); result.side = static_cast<Side>(value.at("side").get<int>());
    result.type = static_cast<OrderType>(value.at("type").get<int>()); result.price = value.at("price").get<Money>();
    result.quantity = value.at("quantity").get<Quantity>(); result.cancel_delay_ns = value.at("cancel_delay_ns").get<Time>();
    result.cancel_clock = static_cast<CancelClock>(value.at("cancel_clock").get<int>()); return result;
}
Command decode_command(const Json& value) {
    Command result; result.scope = decode_scope(value.at("scope")); result.id = value.at("id").get<OrderId>();
    result.intent = decode_intent(value.at("intent")); result.broker_id = value.at("broker_id").get<std::string>();
    result.time_ns = value.at("time_ns").get<Time>(); result.cancel = value.at("cancel").get<bool>(); return result;
}
Json encode(const Report& value) {
    return Json{{"scope", encode(value.scope)}, {"id", value.id}, {"broker_id", value.broker_id},
        {"instrument", encode(value.instrument)}, {"side", static_cast<int>(value.side)},
        {"kind", static_cast<int>(value.kind)}, {"state", static_cast<int>(value.state)},
        {"original", value.original}, {"cumulative", value.cumulative}, {"leaves", value.leaves},
        {"trade_id", value.trade_id}, {"trade_quantity", value.trade_quantity},
        {"cumulative_after", value.cumulative_after}, {"trade_price", value.trade_price},
        {"trade_fee", value.trade_fee}, {"fee_is_final", value.fee_is_final},
        {"error", encode(value.error)}, {"sequence", value.sequence}};
}
Report decode_report(const Json& value) {
    Report result; result.scope = decode_scope(value.at("scope")); result.id = value.at("id").get<OrderId>();
    result.broker_id = value.at("broker_id").get<std::string>(); result.instrument = decode_instrument(value.at("instrument"));
    result.side = static_cast<Side>(value.at("side").get<int>()); result.kind = static_cast<ReportKind>(value.at("kind").get<int>());
    result.state = static_cast<OrderState>(value.at("state").get<int>()); result.original = value.at("original").get<Quantity>();
    result.cumulative = value.at("cumulative").get<Quantity>(); result.leaves = value.at("leaves").get<Quantity>();
    result.trade_id = value.at("trade_id").get<std::string>(); result.trade_quantity = value.at("trade_quantity").get<Quantity>();
    result.cumulative_after = value.at("cumulative_after").get<Quantity>(); result.trade_price = value.at("trade_price").get<Money>();
    result.trade_fee = value.at("trade_fee").get<Money>(); result.fee_is_final = value.at("fee_is_final").get<bool>();
    result.error = decode_error(value.at("error")); result.sequence = value.at("sequence").get<std::uint64_t>(); return result;
}
Json encode(const SnapshotOrder& value) {
    return Json{{"id", value.id}, {"owner", value.owner}, {"broker_id", value.broker_id},
        {"instrument", encode(value.instrument)}, {"side", static_cast<int>(value.side)},
        {"price", value.price}, {"original", value.original}, {"filled", value.filled},
        {"working", value.working}, {"state", static_cast<int>(value.state)}};
}
SnapshotOrder decode_snapshot_order(const Json& value) {
    SnapshotOrder result; result.id = value.at("id").get<OrderId>(); result.owner = value.at("owner").get<std::string>();
    result.broker_id = value.at("broker_id").get<std::string>(); result.instrument = decode_instrument(value.at("instrument"));
    result.side = static_cast<Side>(value.at("side").get<int>()); result.price = value.at("price").get<Money>();
    result.original = value.at("original").get<Quantity>(); result.filled = value.at("filled").get<Quantity>();
    result.working = value.at("working").get<Quantity>(); result.state = static_cast<OrderState>(value.at("state").get<int>());
    return result;
}
}  // namespace

struct Engine::Impl {
    struct TradeFact {
        Quantity quantity = 0, cumulative_after = -1;
        Money price = 0, fee = -1;
        bool after_snapshot = true, fee_final = false;
    };
    struct Order {
        OrderView view;
        Quantity order_cumulative = 0, trade_quantity = 0, settled_filled = 0;
        Quantity new_priced_quantity = 0;
        Money new_amount = 0, new_fees = 0, fee_cap = 0, cash_effect = 0;
        bool dispatched = false, observed = false, cancel_queued = false, cancel_waiting = false;
        bool cancel_epoch_verified = true, has_snapshot_baseline = false;
        Time deadline = -1, cancel_started = -1;
        std::map<std::string, TradeFact> trades;
        std::function<bool()> gate;
    };
    struct Exposure { std::map<Money, Quantity> buy, sell; };
    struct Action { enum Kind { Submit, Cancel, Query } kind; OrderId id; std::uint64_t token; };
    struct Orphan { Report report; Time expires; };
    typedef std::pair<std::uint64_t, std::string> BrokerKey;
    Config config;
    Capabilities caps;
    std::shared_ptr<Backend> backend;
    std::unique_ptr<AccountLease> lease;
    AsyncJournal journal;
    mutable std::mutex mutex;
    std::mutex dispatch_mutex;
    std::map<OrderId, Order> orders;
    std::map<std::pair<std::string, std::string>, OrderId> intents;
    std::map<BrokerKey, OrderId> broker_ids;
    std::map<Instrument, Position> positions;
    std::map<Instrument, Exposure> exposure;
    std::map<std::pair<Time, OrderId>, bool> timers;
    std::deque<Action> actions;
    std::deque<Orphan> orphans;
    std::deque<Time> send_rate, cancel_rate, total_rate;
    std::deque<AuditEvent> audit;
    Time now = 0, new_not_before = 0;
    OrderId next_id = 1, next_external = (1ULL << 63);
    Money cash = 0, reserved = 0;
    std::size_t pending = 0, trade_ids = 0;
    std::uint64_t activity = 0, snapshot_activity = 0, token = 0, last_token = 0;
    std::uint64_t audit_seq = 0, stale = 0, anomalies = 0, admissions = 0, rejections = 0;
    bool connected = false, reconciled = false, reconciling = false, stopping = false;
    bool replaying = false, recovered = false;
    bool have_header = false;
    std::string fault, reason = "connection and reconciliation required";
    Snapshot replay_snapshot;
    bool replay_snapshot_active = false;

    Impl(const Config& value, const std::shared_ptr<Backend>& transport)
        : config(value), caps(transport->capabilities()), backend(transport),
          journal(value.journal_path, 8192, value.audit_sink) {}

    bool ready() const {
        return config.enabled && connected && reconciled && !reconciling && !stopping &&
            fault.empty() && journal.healthy() && orphans.empty() && now >= new_not_before && cash >= reserved;
    }
    void note(const std::string& type, OrderId id, const std::string& detail = std::string()) {
        OMS_PROFILE_SCOPE(profile_note, AuditNote);
        AuditEvent event; event.sequence = ++audit_seq; event.time_ns = now;
        event.order_id = id; event.type = type; event.detail = detail.substr(0, 256);
        if (audit.size() == config.limits.max_audit_events) audit.pop_front();
        audit.push_back(event);
    }
    void freeze(const std::string& detail, OrderId id = 0) {
        if (fault.empty()) fault = detail;
        ++anomalies; note("reconciliation-required", id, detail);
    }
    bool save(const std::string& type, const Json& data, bool durable = false) {
        if (replaying) return true;
        if (!journal.recording()) return save_bytes(std::string(), durable);
        OMS_PROFILE_SCOPE(profile_format, AuditFormat);
        const Json row = {{"v", 1}, {"type", type}, {"time", now}, {"data", data}};
#ifdef USAGI_OMS_PROFILE
        const std::string payload = row.dump();
        OMS_PROFILE_STOP(profile_format);
        const bool appended = journal.append(payload, durable);
#else
        const bool appended = journal.append(row.dump(), durable);
#endif
        if (!appended) {
            freeze("OMS journal failure: " + journal.error()); return false;
        }
        return true;
    }
    bool save_bytes(std::string payload, bool durable = false) {
        if (replaying) return true;
        if (journal.append(std::move(payload), durable)) return true;
        freeze("OMS journal failure: " + journal.error()); return false;
    }
    bool save_id(records::Kind kind, OrderId id, std::uint64_t value = 0, bool durable = false) {
        if (replaying) return true;
        return save_bytes(journal.recording() ? records::id(now, kind, id, value) : std::string(), durable);
    }
    void cancel_timer(Order& order) {
        if (order.deadline >= 0) timers.erase(std::make_pair(order.deadline, order.view.command.id));
        order.deadline = -1;
    }
    void timer(Order& order, Time when) {
        OMS_PROFILE_SCOPE(profile_timer, Timer);
        cancel_timer(order);
        if (order.view.terminal || order.view.cancel_exhausted || replaying) return;
        order.deadline = when; timers[std::make_pair(when, order.view.command.id)] = true;
    }
    void level(const Instrument& instrument, Side side, Money price, Quantity delta) {
        if (!delta) return;
        std::map<Money, Quantity>& levels = side == Side::Buy ? exposure[instrument].buy : exposure[instrument].sell;
        const auto found = levels.find(price);
        const Quantity previous = found == levels.end() ? 0 : found->second;
        const Quantity next = sum(previous, delta);
        if (next < 0) throw std::logic_error("OMS exposure underflow");
        if (!next) levels.erase(price); else levels[price] = next;
    }
    void derive_money(Order& order) {
        OrderView view = order.view;
        view.priced_quantity = 0; view.known_amount = 0; view.known_fees = 0;
        Quantity fee_quantity = 0;
        bool final_fee = false;
        order.new_priced_quantity = 0; order.new_amount = 0; order.new_fees = 0;
        for (const auto& item : order.trades) {
            const TradeFact& fact = item.second;
            if (fact.price > 0) {
                view.priced_quantity = sum(view.priced_quantity, fact.quantity);
                view.known_amount = sum(view.known_amount, product(fact.price, fact.quantity));
                if (fact.after_snapshot) {
                    order.new_priced_quantity = sum(order.new_priced_quantity, fact.quantity);
                    order.new_amount = sum(order.new_amount, product(fact.price, fact.quantity));
                }
            }
            if (fact.fee >= 0) {
                fee_quantity = sum(fee_quantity, fact.quantity);
                view.known_fees = sum(view.known_fees, fact.fee);
                if (fact.after_snapshot) order.new_fees = sum(order.new_fees, fact.fee);
            }
            final_fee = final_fee || fact.fee_final;
        }
        view.amount_complete = view.priced_quantity == view.filled;
        view.fees_complete = (view.terminal && final_fee && fee_quantity == view.filled) ||
            (view.terminal && !order.dispatched && !view.filled);
        const Quantity unpriced = std::max<Quantity>(0,
            view.filled - order.settled_filled - order.new_priced_quantity);
        view.cash_reserved = view.command.intent.side == Side::Buy
            ? product(view.command.intent.price, sum(view.working, unpriced)) : 0;
        if (!view.fees_complete) view.cash_reserved = sum(view.cash_reserved, order.fee_cap);
        order.cash_effect = sum(view.command.intent.side == Side::Buy ? -order.new_amount : order.new_amount,
                                -order.new_fees);
        order.view = view;
    }
    void apply_delta(Order& order, const OrderView& old, Money old_effect) {
        OMS_PROFILE_SCOPE(profile_reservation, Reservation);
        derive_money(order);
        OrderView& view = order.view;
        Position position = positions[view.command.intent.instrument];
        const Quantity fill_delta = view.filled - old.filled;
        if (fill_delta < 0) throw std::logic_error("OMS filled quantity regressed");
        Quantity& working = view.command.intent.side == Side::Buy ? position.working_buy : position.working_sell;
        working = sum(working, view.working - old.working);
        if (working < 0) throw std::logic_error("OMS working reservation underflow");
        if (view.command.intent.side == Side::Buy) {
            position.total = sum(position.total, fill_delta); position.bought = sum(position.bought, fill_delta);
        } else {
            position.total = sum(position.total, -fill_delta); position.sellable = sum(position.sellable, -fill_delta);
            position.sold = sum(position.sold, fill_delta);
        }
        const Money next_cash = sum(cash, difference(order.cash_effect, old_effect));
        const Money next_reserved = sum(reserved, sum(view.cash_reserved, -old.cash_reserved));
        if (next_reserved < 0 || (old.working && !view.working && !pending))
            throw std::logic_error("OMS reservation underflow");
        level(view.command.intent.instrument, view.command.intent.side, view.command.intent.price,
              view.working - old.working);
        positions[view.command.intent.instrument] = position;
        cash = next_cash; reserved = next_reserved;
        if (!old.working && view.working) ++pending;
        if (old.working && !view.working) --pending;
        if (reserved < 0) throw std::logic_error("OMS cash reservation underflow");
        if (position.total < 0 || position.sellable < position.working_sell || cash < reserved)
            freeze("reported execution exceeds account budget", view.command.id);
    }
    void prune(std::deque<Time>& values) {
        while (!values.empty() && now >= values.front() && now - values.front() >= config.limits.rate_window_ns)
            values.pop_front();
    }
    bool rate_available(bool cancel) {
        prune(send_rate); prune(cancel_rate); prune(total_rate);
        return total_rate.size() < config.limits.combined_per_window &&
            (cancel ? cancel_rate.size() < config.limits.cancels_per_window
                    : send_rate.size() < config.limits.new_orders_per_window);
    }
    void take_rate(bool cancel) {
        total_rate.push_back(now); (cancel ? cancel_rate : send_rate).push_back(now);
    }
    bool retime_new_rate(const Order& order) {
        const Time admitted = order.view.command.time_ns;
        if (admitted == now) return true;
        prune(send_rate); prune(cancel_rate); prune(total_rate);
        auto old = std::find(send_rate.begin(), send_rate.end(), admitted);
        if (old != send_rate.end()) {
            send_rate.erase(old);
            old = std::find(total_rate.begin(), total_rate.end(), admitted);
            if (old != total_rate.end()) total_rate.erase(old);
        }
        if (!rate_available(false)) return false;
        take_rate(false); return true;
    }
    bool bind_broker(Order& order, const std::string& id) {
        if (id.empty()) return true;
        if (!text_ok(id)) { freeze("invalid broker ID", order.view.command.id); return false; }
        if (!order.view.command.broker_id.empty() && order.view.command.broker_id != id) {
            freeze("broker ID changed for registered order", order.view.command.id); return false;
        }
        const BrokerKey key(config.scope.epoch, id);
        const auto found = broker_ids.find(key);
        if (found != broker_ids.end() && found->second != order.view.command.id) {
            freeze("ambiguous broker ID in gateway epoch", order.view.command.id); return false;
        }
        order.view.command.broker_id = id; broker_ids[key] = order.view.command.id;
        return true;
    }
    bool apply_report(const Report& report, bool quarantine);
    void resolve_orphans();
    void queue_cancel(Order& order);
    bool install_snapshot(const Snapshot& snapshot, bool persisted);
    bool persist_snapshot(const Snapshot& snapshot);
    bool replay_record(const std::string& payload);
};

Engine::Engine(const Config& config, const std::shared_ptr<Backend>& backend) {
    if (!backend || !text_ok(config.scope.account.broker) || !text_ok(config.scope.account.account) ||
        !text_ok(config.scope.gateway) || !text_ok(config.instance) || config.scope.source <= 0 ||
        config.scope.epoch != 0 || config.instruments.empty())
        throw std::invalid_argument("OMS requires explicit account, gateway, instance, source and instruments");
    const Limits& l = config.limits;
    if (!l.max_orders || !l.max_pending || !l.max_positions || !l.max_trade_ids || !l.max_orphans || !l.max_actions ||
        !l.max_audit_events || !l.new_orders_per_window || !l.cancels_per_window || !l.combined_per_window ||
        l.rate_window_ns <= 0 || l.orphan_timeout_ns <= 0 || l.cancel_retry_ns <= 0 ||
        l.cancel_ack_timeout_ns <= 0 || l.cancel_wait_timeout_ns <= 0 || !l.max_cancel_attempts ||
        l.fee_reserve_per_order < 0)
        throw std::invalid_argument("OMS limits must be positive and bounded");
    std::set<std::string> symbols;
    for (const auto& entry : config.instruments) {
        const InstrumentRules& r = entry.second;
        if (!instrument_ok(entry.first) || r.tick <= 0 || r.lot <= 0 || r.lower_price <= 0 ||
            r.upper_price < r.lower_price || r.max_order_quantity <= 0 || r.max_position <= 0 ||
            r.max_order_notional <= 0)
            throw std::invalid_argument("OMS requires valid instrument trading rules and price bands");
        product(r.upper_price, r.max_order_quantity);
        symbols.insert(entry.first.symbol());
    }
    strategy_runtime::AccountIdentity identity = {config.scope.account.account, config.scope.source, config.scope.day};
    strategy_runtime::AccountReconciliation validate(identity, symbols);
    const Capabilities caps = backend->capabilities();
    if (!config.single_host_account || (!caps.simulated &&
        (config.ownership != OwnershipMode::ExclusiveLocal || config.journal_path.empty() ||
         config.lock_directory != "/run/usagi/oms/accounts")))
        throw std::invalid_argument("real OMS requires single-host exclusive ownership, canonical lease and journal");
    if (config.ownership == OwnershipMode::Simulation && !caps.simulated)
        throw std::invalid_argument("simulation ownership cannot send to a real TD");
    impl_.reset(new Impl(config, backend));
    Impl& s = *impl_;
    if (config.ownership == OwnershipMode::ExclusiveLocal)
        s.lease.reset(new AccountLease(config.lock_directory, config.scope.account.broker, config.scope.account.account));
    s.replaying = true;
    const bool replay_ok = s.journal.replay([&s](const std::string& row) { return s.replay_record(row); });
    s.replaying = false;
    if (!replay_ok) s.freeze("OMS journal replay failed: " + s.journal.error());
    if (!s.journal.sequence()) s.save("header", Json{{"scope", encode(config.scope)}, {"instance", config.instance},
        {"fills", static_cast<int>(s.caps.fills)}, {"fee_reserve", config.limits.fee_reserve_per_order}}, true);
    s.recovered = !s.orders.empty();
    s.connected = s.reconciled = s.reconciling = false;
    s.actions.clear(); s.timers.clear(); s.orphans.clear(); s.broker_ids.clear();
    s.send_rate.clear(); s.cancel_rate.clear(); s.total_rate.clear(); s.now = 0;
    s.new_not_before = s.recovered ? l.rate_window_ns : 0;
    for (auto& item : s.orders) {
        Impl::Order& o = item.second;
        o.deadline = -1; o.cancel_queued = o.cancel_waiting = false;
        o.cancel_epoch_verified = false;
        if (!o.view.terminal) o.view.state = OrderState::Unknown;
    }
}

std::shared_ptr<Engine> Engine::create(const Config& config, const std::shared_ptr<Backend>& backend) {
    std::shared_ptr<Engine> result(new Engine(config, backend));
    std::weak_ptr<Engine> weak(result);
    backend->bind([weak](const Report& report) { if (auto engine = weak.lock()) engine->report(report); },
                  [weak](const Snapshot& snapshot) { if (auto engine = weak.lock()) engine->complete_snapshot(snapshot); },
                  [weak](const Scope& scope, bool connected) { if (auto engine = weak.lock()) engine->set_connected(scope, connected); });
    result->bound_ = true;
    return result;
}

Engine::~Engine() {
    begin_stop();
    if (bound_) impl_->backend->unbind();
    std::lock_guard<std::mutex> dispatch(impl_->dispatch_mutex);
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->journal.sync();
}

Scope Engine::scope() const { std::lock_guard<std::mutex> guard(impl_->mutex); return impl_->config.scope; }
Capabilities Engine::capabilities() const { return impl_->caps; }

bool Engine::start_epoch(std::uint64_t epoch, bool connected) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    Impl& s = *impl_;
    if (!epoch || epoch <= s.config.scope.epoch || s.stopping) return false;
    s.save("epoch", Json{{"epoch", epoch}, {"connected", connected}}, true);
    s.config.scope.epoch = epoch; s.connected = connected; s.reconciled = s.reconciling = false;
    s.reason = "new epoch requires account reconciliation";
    ++s.activity; s.broker_ids.clear(); s.timers.clear(); s.orphans.clear(); s.actions.clear();
    for (auto& item : s.orders) {
        Impl::Order& o = item.second;
        o.cancel_queued = o.cancel_waiting = false; o.cancel_epoch_verified = false; o.deadline = -1;
        if (!o.view.terminal) o.view.state = OrderState::Unknown;
    }
    return true;
}

bool Engine::set_connected(const Scope& scope, bool connected) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    Impl& s = *impl_;
    if (!(scope == s.config.scope)) { ++s.stale; return false; }
    s.save("connected", Json{{"connected", connected}});
    s.connected = connected; ++s.activity;
    if (!connected) { s.reconciled = s.reconciling = false; s.reason = "TD disconnected"; }
    return true;
}

bool Engine::begin_reconcile(std::uint64_t token, bool request_backend) {
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        Impl& s = *impl_;
        if (!s.caps.complete_snapshot) {
            s.reason = "TD cannot certify complete account queries"; s.note("query-unsupported", 0, s.reason); return false;
        }
        if (!s.connected || s.stopping || !token || token <= s.last_token ||
            (request_backend && s.actions.size() >= s.config.limits.max_actions)) return false;
        s.reconciled = false; s.reconciling = true; s.token = s.last_token = token;
        s.snapshot_activity = s.activity; s.reason = "account snapshot in progress";
        s.save("query", Json{{"token", token}});
        if (request_backend) s.actions.push_back(Impl::Action{Impl::Action::Query, 0, token});
    }
    drain(); return true;
}

SubmitResult Engine::submit(const Intent& intent, const std::function<bool()>& gate) {
    OMS_PROFILE_SCOPE(profile_submit, Submit);
    SubmitResult result;
    // The external gate is never invoked with the account mutex held.
    OMS_PROFILE_SCOPE(profile_gate, Gate);
    const bool permitted = gate_allows(gate);
    OMS_PROFILE_STOP(profile_gate);
    {
        OMS_PROFILE_SCOPE(profile_lock, AccountLock);
        std::lock_guard<std::mutex> guard(impl_->mutex);
        OMS_PROFILE_STOP(profile_lock);
        OMS_PROFILE_SCOPE(profile_risk, Risk);
        Impl& s = *impl_;
        const auto reject = [&](ErrorCategory category, const std::string& why) {
            result.error = error(category, why); ++s.rejections; s.note("rejected", 0, why);
            s.save_bytes(s.journal.recording() ? records::rejected(s.now, intent, result.error) : std::string());
        };
        if (!text_ok(intent.owner) || !text_ok(intent.intent_id) || intent.signal_id.size() > 128 ||
            (intent.side != Side::Buy && intent.side != Side::Sell) || intent.price <= 0 || intent.quantity <= 0 ||
            intent.cancel_delay_ns < 0 || (intent.type != OrderType::Limit && intent.type != OrderType::NativeFak &&
            intent.type != OrderType::LimitThenCancel) || (intent.cancel_clock != CancelClock::Submission &&
            intent.cancel_clock != CancelClock::Acceptance)) {
            reject(ErrorCategory::Invalid, "invalid trading intent"); return result;
        }
        if (s.intents.count(std::make_pair(intent.owner, intent.intent_id))) {
            reject(ErrorCategory::Duplicate, "owner/intent ID already registered"); return result;
        }
        if (!permitted || !s.ready()) { reject(ErrorCategory::NotReady, "account or strategy gate closed"); return result; }
        const auto rules = s.config.instruments.find(intent.instrument);
        if (rules == s.config.instruments.end()) { reject(ErrorCategory::Invalid, "instrument not configured"); return result; }
        if (intent.type == OrderType::NativeFak && !s.caps.native_fak) {
            reject(ErrorCategory::Unsupported, "native FAK unsupported by TD"); return result;
        }
        const InstrumentRules& r = rules->second;
        const Position& position = s.positions[intent.instrument];
        try {
            const Money notional = product(intent.price, intent.quantity);
            const Money required = sum(intent.side == Side::Buy ? notional : 0, s.config.limits.fee_reserve_per_order);
            const bool odd_sale = intent.side == Side::Sell && r.allow_odd_lot_liquidation &&
                intent.quantity == position.sellable - position.working_sell;
            if (intent.price < r.lower_price || intent.price > r.upper_price || intent.price % r.tick ||
                (intent.quantity % r.lot && !odd_sale) || intent.quantity > r.max_order_quantity || notional > r.max_order_notional) {
                reject(ErrorCategory::Limit, "price, lot, quantity or notional rule violated"); return result;
            }
            if (intent.side == Side::Buy && sum(sum(position.total, position.working_buy), intent.quantity) > r.max_position) {
                reject(ErrorCategory::Limit, "position limit exceeded"); return result;
            }
            if (required > s.cash - s.reserved) { reject(ErrorCategory::Cash, "insufficient account cash"); return result; }
            if (intent.side == Side::Sell && intent.quantity > position.sellable - position.working_sell) {
                reject(ErrorCategory::Shares, "insufficient account sellable shares"); return result;
            }
            const Impl::Exposure& book = s.exposure[intent.instrument];
            if ((intent.side == Side::Buy && !book.sell.empty() && intent.price >= book.sell.begin()->first) ||
                (intent.side == Side::Sell && !book.buy.empty() && intent.price <= book.buy.rbegin()->first)) {
                reject(ErrorCategory::SelfTrade, "price crosses own or external account order"); return result;
            }
            if (s.orders.size() >= s.config.limits.max_orders || s.pending >= s.config.limits.max_pending ||
                s.actions.size() >= s.config.limits.max_actions || s.next_id >= static_cast<OrderId>(std::numeric_limits<int>::max())) {
                reject(ErrorCategory::Capacity, "OMS order or action capacity reached"); return result;
            }
            if (!s.rate_available(false)) { reject(ErrorCategory::RateLimited, "account order rate reached"); return result; }
            if (intent.type == OrderType::LimitThenCancel) later(s.now, intent.cancel_delay_ns);
            OMS_PROFILE_STOP(profile_risk);
            OMS_PROFILE_SCOPE(profile_construct, OrderConstruction);
            Impl::Order order;
            order.view.command.id = s.next_id; order.view.command.scope = s.config.scope;
            order.view.command.intent = intent; order.view.command.time_ns = s.now;
            order.view.working = intent.quantity; order.fee_cap = s.config.limits.fee_reserve_per_order; order.gate = gate;
            OMS_PROFILE_STOP(profile_construct);
            OMS_PROFILE_SCOPE(profile_intent, IntentAudit);
            if (!s.save_bytes(s.journal.recording() ? records::intent(s.now, order.view.command) : std::string(), true)) {
                reject(ErrorCategory::Persistence, "pre-send durable registration failed"); return result;
            }
            OMS_PROFILE_STOP(profile_intent);
            OrderView empty; s.apply_delta(order, empty, 0);
            OMS_PROFILE_SCOPE(profile_registration, Registration);
            result.id = s.next_id++; result.accepted = true;
            s.orders.emplace(result.id, order); s.intents[std::make_pair(intent.owner, intent.intent_id)] = result.id;
            s.actions.push_back(Impl::Action{Impl::Action::Submit, result.id, 0});
            s.take_rate(false); ++s.admissions; ++s.activity; s.note("registered", result.id, intent.signal_id);
        } catch (const std::exception& e) { s.freeze(e.what()); reject(ErrorCategory::Invalid, e.what()); return result; }
    }
    drain();
    OMS_PROFILE_SCOPE(profile_lookup, FinalLookup);
    OrderView view;
    if (order(result.id, &view) && view.terminal && view.send == SendDisposition::NotSent) {
        result.accepted = false; result.error = view.error;
    }
    return result;
}

void Engine::Impl::queue_cancel(Order& order) {
    if (replaying || order.view.terminal || order.view.cancel_exhausted || order.cancel_queued || order.cancel_waiting) return;
    if (!connected || !order.cancel_epoch_verified ||
        (caps.cancel_requires_broker_id && order.view.command.broker_id.empty())) {
        timer(order, later(now, config.limits.cancel_retry_ns)); return;
    }
    if (actions.size() >= config.limits.max_actions) {
        freeze("cancel action queue full", order.view.command.id);
        timer(order, later(now, config.limits.cancel_retry_ns)); return;
    }
    cancel_timer(order); order.cancel_queued = true;
    actions.push_back(Action{Action::Cancel, order.view.command.id, 0});
}

Error Engine::cancel(const std::string& owner, OrderId id) {
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        Impl& s = *impl_; const auto found = s.orders.find(id);
        if (found == s.orders.end() || !found->second.view.owned || found->second.view.command.intent.owner != owner)
            return error(ErrorCategory::Ownership, "cancel does not own registered order");
        Impl::Order& o = found->second;
        if (o.view.terminal) return error(ErrorCategory::AlreadyFinal, "order is terminal");
        if (o.view.cancel_exhausted) return error(ErrorCategory::Limit, "cancel attempts exhausted; reconciliation required");
        if (!o.view.cancel_requested) {
            s.save_id(records::Kind::CancelIntent, id); o.view.cancel_requested = true; o.cancel_started = s.now;
            s.note("cancel-intent", id);
        }
        s.queue_cancel(o);
    }
    drain(); return Error();
}

bool Engine::schedule_cancel(const std::string& owner, OrderId id, Time delay_ns) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    Impl& s = *impl_; const auto found = s.orders.find(id);
    if (delay_ns < 0 || found == s.orders.end()) return false;
    Impl::Order& o = found->second;
    if (!o.view.owned || o.view.command.intent.owner != owner || o.view.terminal || o.view.cancel_exhausted ||
        o.view.command.intent.type == OrderType::NativeFak) return false;
    try {
        const Time deadline = later(s.now, delay_ns);
        if (o.deadline >= 0 && o.deadline <= deadline) return true;
        s.save_id(records::Kind::Schedule, id, static_cast<std::uint64_t>(deadline)); s.timer(o, deadline); return true;
    } catch (...) { return false; }
}

void Engine::advance_to(Time now_ns) {
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        Impl& s = *impl_;
        if (now_ns < s.now || now_ns < 0) { s.freeze("OMS clock regressed"); return; }
        s.now = now_ns;
        while (!s.orphans.empty() && s.orphans.front().expires <= s.now) {
            s.freeze("unmatched broker report expired", s.orphans.front().report.id); s.orphans.pop_front();
        }
        while (!s.timers.empty() && s.timers.begin()->first.first <= s.now) {
            const OrderId id = s.timers.begin()->first.second; s.timers.erase(s.timers.begin());
            Impl::Order& o = s.orders.at(id); o.deadline = -1;
            if (o.view.terminal || o.view.cancel_exhausted) continue;
            if (!o.view.cancel_requested) {
                s.save_id(records::Kind::CancelIntent, id); o.view.cancel_requested = true; o.cancel_started = s.now;
            }
            if (o.view.cancel_attempts >= s.config.limits.max_cancel_attempts ||
                (o.cancel_started >= 0 && s.now - o.cancel_started >= s.config.limits.cancel_wait_timeout_ns)) {
                o.view.cancel_exhausted = true; o.cancel_waiting = false;
                s.freeze("cancel wait or attempt limit reached", id); continue;
            }
            o.cancel_waiting = false; s.queue_cancel(o);
        }
    }
    impl_->backend->advance_to(now_ns); drain();
}

void Engine::begin_stop() {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->stopping = true; impl_->reason = "OMS stopping";
}

bool Engine::owns(const std::string& owner, OrderId id, const Instrument& instrument) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->orders.find(id);
    return found != impl_->orders.end() && found->second.view.owned &&
        found->second.view.command.intent.owner == owner && found->second.view.command.intent.instrument == instrument;
}

bool Engine::position(const Instrument& instrument, Position* output) const {
    if (!output) return false;
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->positions.find(instrument);
    if (found == impl_->positions.end()) return false;
    *output = found->second; return true;
}

bool Engine::order(OrderId id, OrderView* output) const {
    if (!output) return false;
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->orders.find(id);
    if (found == impl_->orders.end()) return false;
    *output = found->second.view; return true;
}

// Single-flight support for the strategy layer: true when the instrument has
// an order that is still working (not yet Filled/Canceled/Rejected). Used by
// the SSE session to suppress a new signal while one order is in flight.
bool Engine::has_working_order(const Instrument& instrument) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    for (auto it = impl_->orders.begin(); it != impl_->orders.end(); ++it) {
        const Impl::Order& order = it->second;
        if (order.view.command.intent.instrument == instrument && order.view.working > 0 &&
            !terminal(order.view.state))
            return true;
    }
    return false;
}

AccountView Engine::account() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const Impl& s = *impl_; AccountView v;
    v.scope = s.config.scope; v.now_ns = s.now; v.cash_balance = s.cash; v.cash_reserved = s.reserved;
    v.available_cash = s.cash >= s.reserved ? s.cash - s.reserved : 0;
    v.orders = s.orders.size(); v.pending_orders = s.pending;
    v.trade_ids = s.trade_ids; v.pending_actions = s.actions.size(); v.timers = s.timers.size(); v.orphans = s.orphans.size();
    v.ready = s.ready(); v.connected = s.connected; v.stopping = s.stopping;
    v.durable = s.journal.persistent() && s.journal.healthy() && s.journal.sequence() == s.journal.durable_sequence();
    v.audit_sequence = s.journal.sequence();
    v.durable_sequence = s.journal.durable_sequence(); v.stale_reports = s.stale; v.anomalies = s.anomalies;
    v.written_sequence = s.journal.written_sequence(); v.journal_pending = s.journal.pending();
    v.audit_sink_pending = s.journal.sink_pending();
    v.admissions = s.admissions; v.rejections = s.rejections;
    v.reason = !s.journal.healthy() ? "OMS journal failure: " + s.journal.error() :
        !s.fault.empty() ? s.fault : v.ready ? "ready" : s.reason;
    return v;
}

bool Engine::Impl::apply_report(const Report& report, bool quarantine) {
    if (!(report.scope == config.scope)) { ++stale; note("stale-report", report.id); return false; }
    auto found = orders.find(report.id);
    if (!report.id && !report.broker_id.empty()) {
        const auto mapping = broker_ids.find(BrokerKey(config.scope.epoch, report.broker_id));
        if (mapping != broker_ids.end()) found = orders.find(mapping->second);
    }
    if (found == orders.end()) {
        if (quarantine) {
            if (orphans.size() >= config.limits.max_orphans) freeze("orphan report capacity exhausted", report.id);
            else { orphans.push_back(Orphan{report, later(now, config.limits.orphan_timeout_ns)}); note("orphan-report", report.id); }
        }
        return false;
    }
    Order& o = found->second;
    if (!(report.instrument == o.view.command.intent.instrument) || report.side != o.view.command.intent.side ||
        !o.cancel_epoch_verified || (report.original >= 0 && report.original != o.view.command.intent.quantity)) {
        freeze("report identity or original quantity mismatch", found->first); return false;
    }
    if (!bind_broker(o, report.broker_id)) return false;
    const OrderView old = o.view; const Money old_effect = o.cash_effect;
    o.observed = true; o.view.send = SendDisposition::Submitted; o.view.last_report_ns = now;
    o.view.error = report.error;
    if (report.kind == ReportKind::CancelResult) {
        if (o.view.terminal) { note("late-cancel-result", found->first); return true; }
        o.cancel_waiting = false;
        if (report.error.category == ErrorCategory::Temporary || report.error.category == ErrorCategory::RateLimited ||
            report.error.category == ErrorCategory::Unknown || report.error.category == ErrorCategory::MissingId) {
            timer(o, later(now, config.limits.cancel_retry_ns));
        } else if (report.error.failed()) {
            o.view.cancel_exhausted = true; cancel_timer(o); freeze("cancel rejected without terminal order proof", found->first);
        } else if (!o.view.terminal && caps.cancel_acknowledgements) {
            o.cancel_waiting = true; timer(o, later(now, config.limits.cancel_ack_timeout_ns));
        }
        note("cancel-result", found->first, report.error.message); return true;
    }
    Quantity filled = o.view.filled;
    if (report.kind == ReportKind::Trade) {
        if (!text_ok(report.trade_id) || report.trade_quantity <= 0 || report.trade_price < 0 ||
            report.trade_fee < -1 || report.cumulative_after < -1 ||
            (report.cumulative_after >= 0 && report.cumulative_after < report.trade_quantity)) {
            freeze("trade lacks stable identity or valid quantity", found->first); return false;
        }
        auto trade = o.trades.find(report.trade_id);
        if (trade == o.trades.end()) {
            if (trade_ids >= config.limits.max_trade_ids) { freeze("trade identity capacity exhausted", found->first); return false; }
            TradeFact fact; fact.quantity = report.trade_quantity; fact.cumulative_after = report.cumulative_after;
            fact.price = report.trade_price; fact.fee = report.trade_fee; fact.fee_final = report.fee_is_final;
            if (o.has_snapshot_baseline) {
                if (report.cumulative_after < 0) {
                    o.view.quantity_complete = false; freeze("trade overlaps snapshot without cumulative watermark", found->first);
                    // No credit/debit can be inferred for a trade crossing the baseline.
                    fact.after_snapshot = false;
                } else if (report.cumulative_after <= o.settled_filled) fact.after_snapshot = false;
                else if (report.cumulative_after - report.trade_quantity < o.settled_filled) {
                    freeze("trade straddles snapshot settlement watermark", found->first); return false;
                }
            }
            trade = o.trades.emplace(report.trade_id, fact).first;
            o.trade_quantity = sum(o.trade_quantity, fact.quantity); ++trade_ids;
        } else {
            TradeFact& fact = trade->second;
            if (fact.quantity != report.trade_quantity || (fact.cumulative_after >= 0 && report.cumulative_after >= 0 &&
                fact.cumulative_after != report.cumulative_after) || (fact.price && report.trade_price && fact.price != report.trade_price) ||
                (fact.fee >= 0 && report.trade_fee >= 0 && fact.fee != report.trade_fee)) {
                freeze("conflicting duplicate trade", found->first); return false;
            }
            if (!fact.price) fact.price = report.trade_price;
            if (fact.fee < 0) fact.fee = report.trade_fee;
            fact.fee_final = fact.fee_final || report.fee_is_final;
        }
        if (report.cumulative_after >= 0) filled = std::max(filled, report.cumulative_after);
        else if (caps.fills == FillCoverage::TradesFromOrigin || caps.fills == FillCoverage::DualFromOrigin)
            filled = std::max(filled, o.trade_quantity);
        else {
            o.view.quantity_complete = false; freeze("trade/order stream overlap is not provable", found->first);
        }
        if (o.trade_quantity > filled) {
            o.view.quantity_complete = false; freeze("trade coverage exceeds confirmed quantity", found->first);
        }
    } else if (report.kind == ReportKind::Order) {
        if (report.cumulative < 0 || report.leaves < -1 || report.cumulative > o.view.command.intent.quantity ||
            (report.leaves >= 0 && sum(report.cumulative, report.leaves) > o.view.command.intent.quantity)) {
            freeze("invalid or incomplete order quantities", found->first); return false;
        }
        if (!terminal(report.state) && report.state != OrderState::Accepted && report.state != OrderState::Partial &&
            report.state != OrderState::Submitted && report.state != OrderState::CancelPending) {
            freeze("unsupported order status", found->first); return false;
        }
        if (terminal(report.state) && report.cumulative < o.view.filled)
            freeze("terminal order contradicts already confirmed executions", found->first);
        if (report.cumulative < o.order_cumulative) {
            note("cumulative-regression", found->first); ++stale;
            if (o.view.cancel_requested) queue_cancel(o);
            return true;
        }
        o.order_cumulative = report.cumulative;
        filled = std::max(filled, report.cumulative);
        if (report.state == OrderState::Filled && report.cumulative != o.view.command.intent.quantity) {
            freeze("filled status contradicts cumulative quantity", found->first); return false;
        }
    } else { freeze("unknown report kind", found->first); return false; }
    if (filled > o.view.command.intent.quantity) {
        o.view.quantity_complete = false; freeze("execution exceeds original order quantity", found->first);
    }
    if (old.terminal && filled > old.filled) freeze("late execution changes a terminal order", found->first);
    o.view.filled = filled;
    const Quantity remaining = std::max<Quantity>(0, o.view.command.intent.quantity - filled);
    if (old.terminal) {
        o.view.working = 0;
        o.view.canceled = old.state == OrderState::Canceled ? remaining : 0;
        o.view.rejected = old.state == OrderState::Rejected ? remaining : 0;
    } else if (filled >= o.view.command.intent.quantity || (report.kind == ReportKind::Order && terminal(report.state))) {
        o.view.terminal = true; o.view.working = 0;
        o.view.state = filled >= o.view.command.intent.quantity ? OrderState::Filled : report.state;
        o.view.canceled = o.view.state == OrderState::Canceled ? remaining : 0;
        o.view.rejected = o.view.state == OrderState::Rejected ? remaining : 0;
    } else {
        o.view.working = remaining;
        o.view.state = o.view.cancel_requested ? OrderState::CancelPending : filled ? OrderState::Partial : OrderState::Accepted;
        if (report.kind == ReportKind::Order && report.leaves >= 0 && report.leaves != remaining)
            freeze("nonterminal leaves contradict cumulative quantity", found->first);
    }
    if (o.view.accepted_ns < 0) {
        o.view.accepted_ns = now;
        if (o.view.command.intent.type == OrderType::LimitThenCancel &&
            o.view.command.intent.cancel_clock == CancelClock::Acceptance)
            timer(o, later(now, o.view.command.intent.cancel_delay_ns));
    }
    apply_delta(o, old, old_effect);
    if (o.view.terminal) { cancel_timer(o); o.cancel_waiting = o.cancel_queued = false; }
    else if (o.view.cancel_requested) queue_cancel(o);
    note(report.kind == ReportKind::Trade ? "trade-applied" : "order-applied", found->first, state_name(o.view.state));
    return true;
}

void Engine::Impl::resolve_orphans() {
    for (auto it = orphans.begin(); it != orphans.end();) {
        const Report report = it->report;
        if ((report.id && orders.count(report.id)) || (!report.broker_id.empty() &&
            broker_ids.count(BrokerKey(config.scope.epoch, report.broker_id)))) {
            it = orphans.erase(it); apply_report(report, false);
        } else ++it;
    }
}

bool Engine::report(const Report& report) {
    bool result = false;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        Impl& s = *impl_;
        if (!(report.scope == s.config.scope)) { ++s.stale; s.note("stale-report", report.id); return false; }
        ++s.activity;
        try {
            if (report.broker_id.size() > 128 || report.trade_id.size() > 128 ||
                !instrument_ok(report.instrument)) {
                s.freeze("report metadata outside normalized contract", report.id); return false;
            }
            s.save_bytes(s.journal.recording() ? records::report(s.now, report) : std::string());
            result = s.apply_report(report, true);
        }
        catch (const std::exception& e) { s.freeze(e.what(), report.id); }
    }
    drain(); return result;
}

void Engine::drain() {
    OMS_PROFILE_SCOPE(profile_dispatch, Dispatch);
    Impl& s = *impl_;
    OMS_PROFILE_SCOPE(profile_dispatch_lock, DispatcherLock);
    std::unique_lock<std::mutex> dispatch(s.dispatch_mutex, std::try_to_lock);
    OMS_PROFILE_STOP(profile_dispatch_lock);
    if (!dispatch.owns_lock()) return;
    for (;;) {
        Impl::Action action; Command command; Scope query_scope; std::function<bool()> gate;
        {
            OMS_PROFILE_SCOPE(profile_lock, AccountLock);
            std::lock_guard<std::mutex> guard(s.mutex);
            OMS_PROFILE_STOP(profile_lock);
            if (s.actions.empty()) { dispatch.unlock(); return; }
            action = s.actions.front(); s.actions.pop_front();
            if (action.kind == Impl::Action::Query) {
                if (!s.connected || !s.reconciling || action.token != s.token) continue;
                query_scope = s.config.scope;
            } else {
                Impl::Order& o = s.orders.at(action.id);
                if (action.kind == Impl::Action::Cancel) o.cancel_queued = false;
                if (o.view.terminal) continue;
                command = o.view.command; command.scope = s.config.scope; command.time_ns = s.now;
                command.cancel = action.kind == Impl::Action::Cancel; gate = o.gate;
            }
        }
        if (action.kind == Impl::Action::Query) {
            Error result;
            try { result = s.backend->query(query_scope, action.token); }
            catch (...) { result = error(ErrorCategory::Unknown, "TD query threw"); }
            if (result.failed()) {
                std::lock_guard<std::mutex> guard(s.mutex);
                if (query_scope == s.config.scope && action.token == s.token) {
                    s.reconciling = s.reconciled = false; s.reason = result.message;
                    s.note("query-failed", 0, result.message);
                }
            }
            continue;
        }
        const bool permitted = command.cancel || gate_allows(gate);
        bool send = true;
        Error preflight = error(ErrorCategory::NotReady, "gate closed before TD dispatch");
        {
            OMS_PROFILE_SCOPE(profile_lock, AccountLock);
            std::lock_guard<std::mutex> guard(s.mutex);
            OMS_PROFILE_STOP(profile_lock);
            Impl::Order& o = s.orders.at(action.id);
            if (!(command.scope == s.config.scope) || o.view.terminal) continue;
            if (!command.cancel) {
                if (!permitted || !s.ready() || !(o.view.command.scope == s.config.scope)) send = false;
                if (send && !s.retime_new_rate(o)) {
                    send = false; preflight = error(ErrorCategory::RateLimited, "queued order exceeds current dispatch rate");
                }
                // The durable intent already means "possibly sent" after a crash.
                OMS_PROFILE_SCOPE(profile_dispatch_audit, DispatchAudit);
                if (send && !s.save_id(records::Kind::Dispatch, command.id)) send = false;
                OMS_PROFILE_STOP(profile_dispatch_audit);
                if (send) {
                    o.dispatched = true; o.view.send = SendDisposition::Unknown; o.view.state = OrderState::Submitted;
                    if (o.view.command.intent.type == OrderType::LimitThenCancel &&
                        o.view.command.intent.cancel_clock == CancelClock::Submission)
                        s.timer(o, later(s.now, o.view.command.intent.cancel_delay_ns));
                }
            } else {
                if (!s.connected || !o.cancel_epoch_verified ||
                    (s.caps.cancel_requires_broker_id && command.broker_id.empty())) { s.queue_cancel(o); continue; }
                if (o.view.cancel_attempts >= s.config.limits.max_cancel_attempts) {
                    o.view.cancel_exhausted = true; s.freeze("cancel attempts exhausted", action.id); continue;
                }
                if (!s.rate_available(true)) { s.timer(o, later(s.now, s.config.limits.cancel_retry_ns)); continue; }
                ++o.view.cancel_attempts; o.cancel_waiting = true; o.view.state = OrderState::CancelPending;
                s.take_rate(true); s.save_id(records::Kind::CancelDispatch, action.id, o.view.cancel_attempts);
            }
        }
        SendResult result;
        if (!send) result.error = preflight;
        else {
            OMS_PROFILE_SCOPE(profile_backend, BackendCall);
            try { result = command.cancel ? s.backend->cancel(command) : s.backend->submit(command); }
            catch (...) { result.disposition = SendDisposition::Unknown; result.error = error(ErrorCategory::Unknown, "TD send threw; outcome unknown"); }
        }
        {
            OMS_PROFILE_SCOPE(profile_lock, AccountLock);
            std::lock_guard<std::mutex> guard(s.mutex);
            OMS_PROFILE_STOP(profile_lock);
            OMS_PROFILE_SCOPE(profile_result, SendResult);
            Impl::Order& o = s.orders.at(action.id); ++s.activity;
            if (!command.cancel) o.gate = std::function<bool()>();
            OMS_PROFILE_SCOPE(profile_send_audit, SendAudit);
            try {
                s.save_bytes(s.journal.recording() ? records::send(s.now, action.id, result, command.cancel) : std::string());
            } catch (const std::exception&) {
                s.freeze("cannot encode TD send result", action.id);
            }
            OMS_PROFILE_STOP(profile_send_audit);
            if (!(command.scope == s.config.scope)) { s.freeze("TD returned across connection epoch", action.id); continue; }
            s.bind_broker(o, result.broker_id); s.resolve_orphans();
            if (command.cancel) {
                if (o.view.terminal) continue;
                o.view.error = result.error;
                if (result.disposition == SendDisposition::NotSent &&
                    result.error.category != ErrorCategory::Temporary && result.error.category != ErrorCategory::RateLimited &&
                    result.error.category != ErrorCategory::MissingId && result.error.category != ErrorCategory::Unknown) {
                    o.view.cancel_exhausted = true; o.cancel_waiting = false; s.cancel_timer(o);
                    s.freeze("cancel failed without terminal proof", action.id);
                } else if (result.disposition != SendDisposition::Submitted) {
                    o.cancel_waiting = false; s.timer(o, later(s.now, s.config.limits.cancel_retry_ns));
                } else if (s.caps.cancel_acknowledgements && o.deadline < 0)
                    s.timer(o, later(s.now, s.config.limits.cancel_ack_timeout_ns));
            } else {
                const OrderView old = o.view; const Money effect = o.cash_effect;
                if (result.disposition == SendDisposition::NotSent && !o.observed) {
                    o.view.send = result.disposition; o.view.terminal = true; o.view.state = OrderState::Rejected;
                    o.view.rejected = o.view.command.intent.quantity; o.view.working = 0; o.dispatched = false;
                    o.view.error = result.error.failed() ? result.error : error(ErrorCategory::Rejected, "TD did not send order");
                    s.cancel_timer(o); s.apply_delta(o, old, effect);
                } else if (result.disposition == SendDisposition::Unknown && !o.observed) {
                    o.view.send = result.disposition; o.view.state = OrderState::Unknown;
                    o.view.error = result.error; s.freeze("send outcome unknown", action.id);
                } else if (result.disposition == SendDisposition::NotSent && o.observed) {
                    s.freeze("TD reports NotSent after an order report", action.id);
                } else if (!o.observed) { o.view.send = result.disposition; o.view.state = OrderState::Submitted; }
                if (o.view.cancel_requested) s.queue_cancel(o);
            }
            s.note(command.cancel ? "cancel-send" : "send", action.id, error_name(result.error.category));
        }
    }
}

bool Engine::Impl::persist_snapshot(const Snapshot& snapshot) {
    if (!save("snapshot-begin", Json{{"scope", encode(snapshot.scope)}, {"token", snapshot.token},
        {"free_cash", snapshot.free_cash}, {"account_success", snapshot.account_success},
        {"positions_success", snapshot.positions_success}, {"orders_success", snapshot.orders_success},
        {"trades_success", snapshot.trades_success}, {"all_day_orders", snapshot.all_day_orders},
        {"all_day_trades", snapshot.all_day_trades}})) return false;
    for (const auto& p : snapshot.positions)
        if (!save("snapshot-position", Json{{"instrument", encode(p.instrument)}, {"total", p.total}, {"free_sellable", p.free_sellable}})) return false;
    for (const auto& o : snapshot.orders) if (!save("snapshot-order", encode(o))) return false;
    for (const auto& t : snapshot.trades) if (!save("snapshot-trade", encode(t))) return false;
    return save("snapshot-commit", Json{{"token", snapshot.token}}, true);
}

bool Engine::Impl::install_snapshot(const Snapshot& snapshot, bool persisted) {
    const auto reject = [&](const std::string& why) { reconciled = false; reconciling = false; reason = why; note("snapshot-rejected", 0, why); return false; };
    if (!snapshot.account_success || !snapshot.positions_success || !snapshot.orders_success ||
        (caps.trades_required_for_snapshot && (!snapshot.trades_success || !snapshot.all_day_trades)) ||
        snapshot.free_cash < 0 || snapshot.orders.size() > config.limits.max_orders ||
        snapshot.trades.size() > config.limits.max_trade_ids || snapshot.positions.size() > config.limits.max_positions)
        return reject("incomplete, failed or oversized account snapshot");
    std::map<Instrument, Position> new_positions;
    for (const auto& p : snapshot.positions) {
        if (!instrument_ok(p.instrument) || p.total < 0 || p.free_sellable < 0 || p.free_sellable > p.total ||
            new_positions.count(p.instrument)) return reject("invalid or duplicate snapshot position");
        Position position; position.total = p.total; position.sellable = p.free_sellable;
        new_positions[p.instrument] = position;
    }
    for (const auto& r : config.instruments)
        if (!new_positions.count(r.first)) return reject("snapshot does not cover configured universe");
    std::map<OrderId, Order> new_orders;
    for (const auto& item : orders) if (item.second.view.owned) new_orders.insert(item);
    std::set<OrderId> seen_owned;
    std::map<BrokerKey, OrderId> new_broker_ids;
    Money new_cash = snapshot.free_cash;
    OrderId external_id = (1ULL << 63);
    for (const auto& row : snapshot.orders) {
        if (!instrument_ok(row.instrument) || !new_positions.count(row.instrument) || !text_ok(row.broker_id) ||
            row.price <= 0 || row.original <= 0 || row.filled < 0 || row.working < 0 ||
            row.filled > row.original || row.working > row.original - row.filled ||
            (row.side != Side::Buy && row.side != Side::Sell) ||
            (!terminal(row.state) && row.state != OrderState::Accepted && row.state != OrderState::Partial &&
             row.state != OrderState::Submitted && row.state != OrderState::CancelPending) ||
            (terminal(row.state) ? row.working != 0 : row.working != row.original - row.filled) ||
            (row.state == OrderState::Filled && row.filled != row.original))
            return reject("invalid snapshot order");
        OrderId id = row.id;
        auto own = new_orders.find(id);
        const bool owned = own != new_orders.end() && own->second.view.owned;
        if (owned) {
            const Intent& intent = own->second.view.command.intent;
            if (intent.owner != row.owner || !(intent.instrument == row.instrument) || intent.side != row.side ||
                intent.price != row.price || intent.quantity != row.original || row.filled < own->second.view.filled ||
                (!own->second.view.command.broker_id.empty() && own->second.view.command.broker_id != row.broker_id) ||
                !seen_owned.insert(id).second) return reject("snapshot cannot prove historical order ownership");
        } else {
            id = external_id++; Order external;
            external.view.owned = false; external.view.command.id = id;
            external.view.command.intent.owner = "external"; external.view.command.intent.intent_id = row.broker_id;
            external.view.command.intent.instrument = row.instrument; external.view.command.intent.side = row.side;
            external.view.command.intent.price = row.price; external.view.command.intent.quantity = row.original;
            own = new_orders.emplace(id, external).first;
        }
        const BrokerKey broker(config.scope.epoch, row.broker_id);
        if (!new_broker_ids.emplace(broker, id).second) return reject("duplicate snapshot broker identity");
        Order& order = own->second;
        order.view.command.scope = config.scope; order.view.command.broker_id = row.broker_id;
        order.view.filled = row.filled; order.view.working = row.working;
        order.view.state = row.state; order.view.terminal = terminal(row.state);
        order.view.canceled = row.state == OrderState::Canceled ? row.original - row.filled : 0;
        order.view.rejected = row.state == OrderState::Rejected ? row.original - row.filled : 0;
        order.view.send = SendDisposition::Submitted; order.view.quantity_complete = true;
        order.order_cumulative = row.filled; order.settled_filled = row.filled;
        order.has_snapshot_baseline = true; order.cancel_epoch_verified = true;
        order.dispatched = order.observed = true;
        order.fee_cap = row.working ? config.limits.fee_reserve_per_order : 0;
        order.deadline = -1; order.cancel_waiting = order.cancel_queued = false;
        for (auto& trade : order.trades) trade.second.after_snapshot = false;
        if (row.side == Side::Buy) new_cash = sum(new_cash, product(row.price, row.working));
        else new_positions[row.instrument].sellable = sum(new_positions[row.instrument].sellable, row.working);
    }
    bool unresolved = false;
    for (auto& item : new_orders) {
        Order& o = item.second;
        if (!o.view.owned || seen_owned.count(item.first)) continue;
        o.settled_filled = o.view.filled; o.has_snapshot_baseline = true;
        for (auto& trade : o.trades) trade.second.after_snapshot = false;
        o.deadline = -1; o.cancel_queued = o.cancel_waiting = false;
        if (!o.view.terminal) {
            if ((!o.dispatched && !o.observed) || (caps.absence_proves_unsent && snapshot.all_day_orders && snapshot.all_day_trades)) {
                o.view.terminal = true; o.view.state = OrderState::Rejected; o.view.working = 0;
                o.view.rejected = o.view.command.intent.quantity - o.view.filled; o.view.send = SendDisposition::NotSent;
                o.fee_cap = 0;
            } else { unresolved = true; o.view.state = OrderState::Unknown; o.cancel_epoch_verified = false; }
        } else o.fee_cap = 0;
    }
    if (new_orders.size() > config.limits.max_orders) return reject("restored orders exceed OMS capacity");
    for (const auto& row : snapshot.trades) {
        if (!(row.scope == snapshot.scope) || row.kind != ReportKind::Trade || !text_ok(row.trade_id) ||
            row.trade_quantity <= 0 || row.trade_price < 0 || row.trade_fee < -1)
            return reject("invalid snapshot trade");
        const auto broker = new_broker_ids.find(BrokerKey(config.scope.epoch, row.broker_id));
        if (broker == new_broker_ids.end()) return reject("snapshot trade has no historical order");
        Order& o = new_orders.at(broker->second);
        if (!(row.instrument == o.view.command.intent.instrument) || row.side != o.view.command.intent.side ||
            (row.id && row.id != o.view.command.id && o.view.owned) ||
            (row.cumulative_after >= 0 && row.cumulative_after > o.view.filled))
            return reject("snapshot trade identity or watermark mismatch");
        const auto old = o.trades.find(row.trade_id);
        if (old != o.trades.end() && (old->second.quantity != row.trade_quantity ||
            (old->second.price && row.trade_price && old->second.price != row.trade_price) ||
            (old->second.fee >= 0 && row.trade_fee >= 0 && old->second.fee != row.trade_fee) ||
            (old->second.cumulative_after >= 0 && row.cumulative_after >= 0 && old->second.cumulative_after != row.cumulative_after)))
            return reject("snapshot trade contradicts durable history");
        TradeFact& fact = o.trades[row.trade_id]; fact.quantity = row.trade_quantity;
        if (row.trade_price) fact.price = row.trade_price;
        if (row.trade_fee >= 0) fact.fee = row.trade_fee;
        if (row.cumulative_after >= 0) fact.cumulative_after = row.cumulative_after;
        fact.after_snapshot = false; fact.fee_final = fact.fee_final || row.fee_is_final;
    }
    Money new_reserved = 0;
    std::size_t new_pending = 0, new_trade_ids = 0;
    std::map<Instrument, Exposure> new_exposure;
    for (auto& item : new_orders) {
        Order& o = item.second; o.trade_quantity = 0;
        for (const auto& t : o.trades) o.trade_quantity = sum(o.trade_quantity, t.second.quantity);
        if (o.trade_quantity > o.view.filled) return reject("snapshot trade coverage exceeds confirmed fills");
        if (caps.trades_required_for_snapshot && o.trade_quantity != o.view.filled)
            return reject("complete trade query does not cover confirmed executions");
        derive_money(o); new_reserved = sum(new_reserved, o.view.cash_reserved);
        new_trade_ids += o.trades.size();
        Position& p = new_positions[o.view.command.intent.instrument];
        if (o.view.command.intent.side == Side::Buy) p.working_buy = sum(p.working_buy, o.view.working);
        else p.working_sell = sum(p.working_sell, o.view.working);
        if (o.view.working) {
            ++new_pending;
            Exposure& book = new_exposure[o.view.command.intent.instrument];
            auto& levels = o.view.command.intent.side == Side::Buy ? book.buy : book.sell;
            levels[o.view.command.intent.price] = sum(levels[o.view.command.intent.price], o.view.working);
        }
    }
    if (new_trade_ids > config.limits.max_trade_ids || new_pending > config.limits.max_pending)
        return reject("snapshot risk state exceeds OMS capacity");
    for (const auto& p : new_positions)
        if (p.second.sellable > p.second.total || p.second.working_sell > p.second.sellable) unresolved = true;
    if (!persisted && !persist_snapshot(snapshot)) return reject("snapshot durability failed");
    orders.swap(new_orders); positions.swap(new_positions); exposure.swap(new_exposure); broker_ids.swap(new_broker_ids);
    cash = new_cash; reserved = new_reserved; pending = new_pending; trade_ids = new_trade_ids; next_external = external_id;
    timers.clear(); actions.clear(); orphans.clear(); fault.clear();
    reconciled = !unresolved && journal.healthy() && cash >= reserved; reconciling = false;
    reason = reconciled ? "ready" : "snapshot retains unresolved orders or insufficient account budget";
    if (unresolved) freeze(reason);
    note("snapshot-installed", 0, reason);
    for (auto& item : orders)
        if (item.second.view.owned && item.second.view.cancel_requested) queue_cancel(item.second);
    return reconciled;
}

bool Engine::complete_snapshot(const Snapshot& snapshot) {
    bool result = false;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex); Impl& s = *impl_;
        if (!(snapshot.scope == s.config.scope) || !s.connected || !s.reconciling || snapshot.token != s.token) {
            ++s.stale; return false;
        }
        if (s.activity != s.snapshot_activity) {
            s.reconciling = false; s.reconciled = false; s.reason = "account activity overlapped snapshot"; return false;
        }
        try { result = s.install_snapshot(snapshot, false); }
        catch (const std::exception& e) { s.reconciling = false; s.freeze(e.what()); }
    }
    drain(); return result;
}

bool Engine::Impl::replay_record(const std::string& payload) {
    const Json row = records::decode(payload);
    if (row.at("v").get<int>() != 1) return false;
    const std::string type = row.at("type").get<std::string>();
    const Json& data = row.at("data"); now = row.at("time").get<Time>();
    if (type == "header") {
        if (have_header) return false;
        Scope scope = decode_scope(data.at("scope")); scope.epoch = config.scope.epoch;
        have_header = scope == config.scope && data.at("instance").get<std::string>() == config.instance &&
            data.at("fills").get<int>() == static_cast<int>(caps.fills) &&
            data.at("fee_reserve").get<Money>() == config.limits.fee_reserve_per_order;
        return have_header;
    }
    if (!have_header) return false;
    if (type == "epoch") {
        config.scope.epoch = data.at("epoch").get<std::uint64_t>();
        connected = data.at("connected").get<bool>(); broker_ids.clear();
        for (auto& o : orders) o.second.cancel_epoch_verified = false;
    } else if (type == "connected") connected = data.at("connected").get<bool>();
    else if (type == "query") last_token = data.at("token").get<std::uint64_t>();
    else if (type == "intent") {
        Order order; order.view.command = decode_command(data);
        order.dispatched = true;
        const auto key = std::make_pair(order.view.command.intent.owner, order.view.command.intent.intent_id);
        if (!order.view.command.id || orders.count(order.view.command.id) || intents.count(key) ||
            orders.size() >= config.limits.max_orders) return false;
        order.view.working = order.view.command.intent.quantity; order.fee_cap = config.limits.fee_reserve_per_order;
        OrderView empty; apply_delta(order, empty, 0);
        next_id = std::max(next_id, order.view.command.id + 1); intents[key] = order.view.command.id;
        orders.emplace(order.view.command.id, order);
    } else if (type == "dispatch") {
        Order& o = orders.at(data.at("id").get<OrderId>()); o.dispatched = true;
        o.view.state = OrderState::Unknown; o.view.send = SendDisposition::Unknown;
    } else if (type == "report") apply_report(decode_report(data), true);
    else if (type == "send") {
        Order& o = orders.at(data.at("id").get<OrderId>());
        const SendDisposition disposition = static_cast<SendDisposition>(data.at("disposition").get<int>());
        bind_broker(o, data.at("broker_id").get<std::string>()); resolve_orphans();
        if (!o.observed) {
            const OrderView old = o.view; const Money effect = o.cash_effect;
            o.view.send = disposition; o.view.error = decode_error(data.at("error"));
            if (disposition == SendDisposition::NotSent) {
                o.dispatched = false; o.view.terminal = true; o.view.state = OrderState::Rejected;
                o.view.rejected = o.view.command.intent.quantity; o.view.working = 0; apply_delta(o, old, effect);
            } else o.view.state = disposition == SendDisposition::Unknown ? OrderState::Unknown : OrderState::Submitted;
        }
    } else if (type == "cancel-intent") {
        Order& o = orders.at(data.at("id").get<OrderId>()); o.view.cancel_requested = true; o.cancel_started = now;
    } else if (type == "cancel-dispatch") {
        orders.at(data.at("id").get<OrderId>()).view.cancel_attempts = data.at("attempt").get<unsigned>();
    } else if (type == "rejected") ++rejections;
    else if (type == "cancel-send" || type == "schedule") {
        // Monotonic deadlines cannot be resumed across process boots.
    } else if (type == "snapshot-begin") {
        replay_snapshot = Snapshot(); replay_snapshot_active = true;
        replay_snapshot.scope = decode_scope(data.at("scope")); replay_snapshot.token = data.at("token").get<std::uint64_t>();
        replay_snapshot.free_cash = data.at("free_cash").get<Money>();
        replay_snapshot.account_success = data.at("account_success").get<bool>();
        replay_snapshot.positions_success = data.at("positions_success").get<bool>();
        replay_snapshot.orders_success = data.at("orders_success").get<bool>();
        replay_snapshot.trades_success = data.at("trades_success").get<bool>();
        replay_snapshot.all_day_orders = data.at("all_day_orders").get<bool>();
        replay_snapshot.all_day_trades = data.at("all_day_trades").get<bool>();
    } else if (type == "snapshot-position" && replay_snapshot_active) {
        if (replay_snapshot.positions.size() >= config.limits.max_positions) return false;
        SnapshotPosition p; p.instrument = decode_instrument(data.at("instrument"));
        p.total = data.at("total").get<Quantity>(); p.free_sellable = data.at("free_sellable").get<Quantity>();
        replay_snapshot.positions.push_back(p);
    } else if (type == "snapshot-order" && replay_snapshot_active) {
        if (replay_snapshot.orders.size() >= config.limits.max_orders) return false;
        replay_snapshot.orders.push_back(decode_snapshot_order(data));
    } else if (type == "snapshot-trade" && replay_snapshot_active) {
        if (replay_snapshot.trades.size() >= config.limits.max_trade_ids) return false;
        replay_snapshot.trades.push_back(decode_report(data));
    } else if (type == "snapshot-commit" && replay_snapshot_active &&
               data.at("token").get<std::uint64_t>() == replay_snapshot.token) {
        install_snapshot(replay_snapshot, true); replay_snapshot_active = false;
    } else return false;
    return true;
}

std::vector<AuditEvent> Engine::audit_events() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return std::vector<AuditEvent>(impl_->audit.begin(), impl_->audit.end());
}

}  // namespace oms
