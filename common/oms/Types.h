#ifndef USAGI_OMS_TYPES_H
#define USAGI_OMS_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace oms {

typedef std::uint64_t OrderId;
typedef std::int64_t Quantity;
typedef std::int64_t Money;
typedef std::int64_t Time;
static const Money kMoneyScale = 10000;

enum class Side { Buy, Sell };
enum class OrderType { Limit, NativeFak, LimitThenCancel };
enum class SendDisposition { NotSent, Submitted, Unknown };
enum class OrderState { Queued, Submitted, Unknown, Accepted, Partial,
                        CancelPending, Canceled, Rejected, Filled, Reconcile };
enum class ErrorCategory { None, Invalid, Ownership, Duplicate, NotReady,
    Cash, Shares, Limit, SelfTrade, RateLimited, Unsupported, Temporary,
    MissingId, AlreadyFinal, Rejected, Unknown, Capacity, Persistence, Protocol };
// FromOrigin requires a complete, gap-free execution-ordered trade prefix.
enum class FillCoverage { Cumulative, TradesFromOrigin, DualFromOrigin, UnknownOverlap };
enum class CancelClock { Submission, Acceptance };
enum class ReportKind { Order, Trade, CancelResult };
enum class OwnershipMode { Simulation, ExclusiveLocal };

struct Instrument {
    std::string market;
    std::string code;
    bool operator<(const Instrument& rhs) const {
        return std::tie(market, code) < std::tie(rhs.market, rhs.code);
    }
    bool operator==(const Instrument& rhs) const {
        return market == rhs.market && code == rhs.code;
    }
    std::string symbol() const { return code + (market == "SSE" ? ".SH" : ".SZ"); }
};

struct AccountKey {
    std::string broker;
    std::string account;
    bool operator==(const AccountKey& rhs) const {
        return broker == rhs.broker && account == rhs.account;
    }
};

struct Scope {
    AccountKey account;
    std::string gateway;
    std::uint32_t day = 0;
    std::uint64_t epoch = 0;
    short source = 0;
    bool operator==(const Scope& rhs) const {
        return account == rhs.account && gateway == rhs.gateway && day == rhs.day &&
               epoch == rhs.epoch && source == rhs.source;
    }
};

struct Error {
    ErrorCategory category = ErrorCategory::None;
    int raw_code = 0;
    std::string message;
    std::string raw_type;
    bool failed() const { return category != ErrorCategory::None; }
};

struct Capabilities {
    bool simulated = false;
    bool native_fak = false;
    bool cancel_requires_broker_id = true;
    bool cancel_acknowledgements = true;
    bool complete_snapshot = false;
    bool absence_proves_unsent = false;
    bool trades_required_for_snapshot = true;
    FillCoverage fills = FillCoverage::UnknownOverlap;
};

struct InstrumentRules {
    Money tick = 100;
    Money lower_price = 0;
    Money upper_price = 0;
    Quantity lot = 100;
    Quantity max_order_quantity = 1000000;
    Quantity max_position = 2147483647;
    Money max_order_notional = 10000000000LL;
    bool allow_odd_lot_liquidation = false;
};

struct Limits {
    std::size_t max_orders = 100000;
    std::size_t max_pending = 10000;
    std::size_t max_positions = 100000;
    std::size_t max_trade_ids = 1000000;
    std::size_t max_orphans = 1024;
    std::size_t max_actions = 20000;
    std::size_t max_audit_events = 1024;
    std::size_t new_orders_per_window = 1000;
    std::size_t cancels_per_window = 1000;
    std::size_t combined_per_window = 2000;
    Time rate_window_ns = 1000000000LL;
    Time orphan_timeout_ns = 1000000000LL;
    Time cancel_retry_ns = 100000000LL;
    Time cancel_ack_timeout_ns = 1000000000LL;
    Time cancel_wait_timeout_ns = 30000000000LL;
    unsigned max_cancel_attempts = 5;
    Money fee_reserve_per_order = 0;
};

struct Config {
    Scope scope;
    std::string instance;
    OwnershipMode ownership = OwnershipMode::Simulation;
    std::string journal_path;
    // Optional export sink. Runs on a separate worker; throw on output failure.
    // Payload is a compact record (legacy JSON for cold events), not printable text.
    std::function<void(const std::string&)> audit_sink;
    std::string lock_directory;
    bool single_host_account = true;
    bool enabled = false;
    // When true, a fresh start/recovery that restores working orders first
    // cancels every restored open order and keeps the account not ready until
    // those cancels are terminal (restart-cancel-then-trade, OMS-3).
    bool restart_cancel_open_orders = false;
    // False: reserve order IDs durably at startup, reconcile on restart,
    // and enqueue individual order records without waiting for disk sync.
    bool durable_order_intents = true;
    std::map<Instrument, InstrumentRules> instruments;
    Limits limits;
};

struct Intent {
    std::string owner;
    std::string intent_id;
    std::string signal_id;
    Instrument instrument;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Money price = 0;
    Quantity quantity = 0;
    Time cancel_delay_ns = 0;
    CancelClock cancel_clock = CancelClock::Submission;
};

struct Command {
    Scope scope;
    OrderId id = 0;
    Intent intent;
    std::string broker_id;
    Time time_ns = 0;
    bool cancel = false;
};

struct SendResult {
    SendDisposition disposition = SendDisposition::NotSent;
    std::string broker_id;
    Error error;
};

struct Report {
    Scope scope;
    OrderId id = 0;
    std::string broker_id;
    Instrument instrument;
    Side side = Side::Buy;
    ReportKind kind = ReportKind::Order;
    OrderState state = OrderState::Accepted;
    Quantity original = -1;
    Quantity cumulative = -1;
    Quantity leaves = -1;
    std::string trade_id;
    Quantity trade_quantity = 0;
    Quantity cumulative_after = -1;
    Money trade_price = 0;
    Money trade_fee = -1;
    bool fee_is_final = false;
    Error error;
    std::uint64_t sequence = 0;
};

struct Position {
    Quantity total = 0;
    // Sellable before outstanding sell reservations; same-day buys do not add to it.
    Quantity sellable = 0;
    Quantity working_buy = 0;
    Quantity working_sell = 0;
    Quantity bought = 0;
    Quantity sold = 0;
};

struct OrderView {
    Command command;
    OrderState state = OrderState::Queued;
    SendDisposition send = SendDisposition::NotSent;
    Quantity filled = 0;
    Quantity working = 0;
    Quantity canceled = 0;
    Quantity rejected = 0;
    Quantity priced_quantity = 0;
    Money known_amount = 0;
    Money known_fees = 0;
    Money cash_reserved = 0;
    bool quantity_complete = true;
    bool amount_complete = false;
    bool fees_complete = false;
    bool owned = true;
    bool terminal = false;
    bool cancel_requested = false;
    bool cancel_exhausted = false;
    unsigned cancel_attempts = 0;
    Time accepted_ns = -1;
    Time last_report_ns = 0;
    Error error;
};

struct AccountView {
    Scope scope;
    Time now_ns = 0;
    Money cash_balance = 0;
    Money cash_reserved = 0;
    Money available_cash = 0;
    std::size_t orders = 0;
    std::size_t pending_orders = 0;
    std::size_t trade_ids = 0;
    std::size_t pending_actions = 0;
    std::size_t timers = 0;
    std::size_t orphans = 0;
    bool ready = false;
    bool connected = false;
    bool stopping = false;
    bool durable = false;
    std::uint64_t audit_sequence = 0;
    std::uint64_t durable_sequence = 0;
    std::uint64_t written_sequence = 0;
    std::size_t journal_pending = 0;
    std::size_t audit_sink_pending = 0;
    std::uint64_t stale_reports = 0;
    std::uint64_t anomalies = 0;
    std::uint64_t admissions = 0;
    std::uint64_t rejections = 0;
    std::string reason;
};

struct SubmitResult {
    OrderId id = 0;
    bool accepted = false;
    Error error;
};

struct SnapshotPosition {
    Instrument instrument;
    Quantity total = 0;
    // Query reports free sellable shares after all current sell reservations.
    Quantity free_sellable = 0;
};

struct SnapshotOrder {
    OrderId id = 0;
    std::string owner;
    std::string broker_id;
    Instrument instrument;
    Side side = Side::Buy;
    Money price = 0;
    Quantity original = 0;
    Quantity filled = 0;
    Quantity working = 0;
    OrderState state = OrderState::Accepted;
};

struct Snapshot {
    Scope scope;
    std::uint64_t token = 0;
    Money free_cash = 0;
    bool account_success = false;
    bool positions_success = false;
    bool orders_success = false;
    bool trades_success = false;
    bool all_day_orders = false;
    bool all_day_trades = false;
    std::vector<SnapshotPosition> positions;
    std::vector<SnapshotOrder> orders;
    std::vector<Report> trades;
};

struct AuditEvent {
    std::uint64_t sequence = 0;
    Time time_ns = 0;
    OrderId order_id = 0;
    std::string type;
    std::string detail;
};

bool money_from_double(double value, Money* output);
const char* state_name(OrderState value);
const char* error_name(ErrorCategory value);

}  // namespace oms
#endif
