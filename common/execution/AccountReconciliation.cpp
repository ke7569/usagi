#include "common/execution/AccountReconciliation.h"

#include <cmath>
#include <stdexcept>

namespace strategy_runtime {
namespace {

bool valid_day(std::uint32_t value) {
    const unsigned year = value / 10000U;
    const unsigned month = value / 100U % 100U;
    const unsigned day = value % 100U;
    if (year < 2000U || year > 9999U || month < 1U || month > 12U || day < 1U)
        return false;
    const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
    return day <= days[month - 1U] + (month == 2U && leap ? 1U : 0U);
}

}  // namespace

AccountReconciliation::AccountReconciliation(const AccountIdentity& identity,
        const std::set<std::string>& instruments)
    : identity_(identity), instruments_(instruments), epoch_(0), snapshot_token_(0),
      latest_token_(0), connected_(false), stopped_(false), snapshot_active_(false),
      snapshot_invalid_(false), account_seen_(false), positions_finished_(false),
      orders_finished_(false), snapshot_finished_(false), available_cash_(0),
      failure_reason_("reconciliation epoch not begun") {
    if (identity.account.empty() || identity.source <= 0 || !valid_day(identity.day) ||
        instruments.empty() || instruments.count(std::string()))
        throw std::invalid_argument("reconciliation requires an account, source, day and nonempty universe");
}

bool AccountReconciliation::reject(const std::string& reason) {
    last_rejection_ = reason;
    return false;
}

bool AccountReconciliation::invalidate(const std::string& reason) {
    snapshot_invalid_ = true;
    snapshot_active_ = false;
    snapshot_finished_ = false;
    failure_reason_ = reason;
    return reject(reason);
}

bool AccountReconciliation::current_epoch(std::uint64_t epoch) {
    if (stopped_) return reject("reconciliation stopped");
    if (epoch == 0 || epoch != epoch_) return reject("reconciliation epoch mismatch");
    return true;
}

bool AccountReconciliation::current_snapshot(std::uint64_t epoch, std::uint64_t token) {
    if (!current_epoch(epoch)) return false;
    if (token == 0 || token != snapshot_token_)
        return reject("reconciliation snapshot token mismatch");
    if (!connected_) return reject("reconciliation disconnected");
    if (snapshot_invalid_) return reject("reconciliation snapshot already invalid");
    if (!snapshot_active_) return invalidate("reconciliation snapshot already finished");
    return true;
}

bool AccountReconciliation::same_identity(const AccountIdentity& identity) const {
    return identity.account == identity_.account && identity.source == identity_.source &&
           identity.day == identity_.day;
}

void AccountReconciliation::clear_snapshot() {
    positions_.clear();
    snapshot_token_ = 0;
    snapshot_active_ = false;
    snapshot_invalid_ = false;
    account_seen_ = false;
    positions_finished_ = false;
    orders_finished_ = false;
    snapshot_finished_ = false;
    available_cash_ = 0;
    last_rejection_.clear();
}

bool AccountReconciliation::begin_epoch(std::uint64_t epoch) {
    if (stopped_) return reject("reconciliation stopped");
    if (epoch == 0 || epoch <= epoch_) return reject("reconciliation epoch must increase");
    epoch_ = epoch;
    connected_ = false;
    clear_snapshot();
    failure_reason_ = "reconciliation disconnected";
    return true;
}

bool AccountReconciliation::mark_connected(std::uint64_t epoch, bool connected) {
    if (!current_epoch(epoch)) return false;
    connected_ = connected;
    if (!connected) {
        invalidate("reconciliation disconnected");
        return true;
    }
    if (!snapshot_invalid_ && !snapshot_active_ && !snapshot_finished_)
        failure_reason_ = "reconciliation snapshot not begun";
    return true;
}

bool AccountReconciliation::begin_snapshot(std::uint64_t epoch, std::uint64_t token) {
    if (!current_epoch(epoch)) return false;
    if (!connected_) return reject("reconciliation disconnected");
    if (token == 0 || token <= latest_token_)
        return reject("reconciliation snapshot token must increase");
    clear_snapshot();
    snapshot_token_ = token;
    latest_token_ = token;
    snapshot_active_ = true;
    failure_reason_ = "reconciliation snapshot incomplete";
    return true;
}

bool AccountReconciliation::add_account(std::uint64_t epoch, std::uint64_t token,
        const AccountIdentity& identity, double available_cash) {
    if (!current_snapshot(epoch, token)) return false;
    if (!same_identity(identity)) return invalidate("reconciliation account identity mismatch");
    if (account_seen_) return invalidate("duplicate reconciliation account row");
    if (!std::isfinite(available_cash) || available_cash < 0)
        return invalidate("invalid reconciliation available cash");
    account_seen_ = true;
    available_cash_ = available_cash;
    return true;
}

bool AccountReconciliation::add_position(std::uint64_t epoch, std::uint64_t token,
        const AccountIdentity& identity, const std::string& instrument, int total, int available) {
    if (!current_snapshot(epoch, token)) return false;
    if (!same_identity(identity)) return invalidate("reconciliation position identity mismatch");
    if (positions_finished_) return invalidate("position row after reconciliation query finished");
    if (!instruments_.count(instrument)) return invalidate("reconciliation position outside universe");
    if (total < 0 || available < 0 || available > total)
        return invalidate("invalid reconciliation position quantities");
    const PositionSnapshot value = {total, available};
    if (!positions_.insert(std::make_pair(instrument, value)).second)
        return invalidate("duplicate reconciliation position row");
    return true;
}

bool AccountReconciliation::finish_positions(std::uint64_t epoch, std::uint64_t token) {
    if (!current_snapshot(epoch, token)) return false;
    if (positions_finished_) return invalidate("duplicate reconciliation positions completion");
    if (positions_.size() != instruments_.size())
        return invalidate("reconciliation positions query has missing instruments");
    positions_finished_ = true;
    return true;
}

bool AccountReconciliation::finish_orders(std::uint64_t epoch, std::uint64_t token,
        std::size_t open_orders) {
    if (!current_snapshot(epoch, token)) return false;
    if (orders_finished_) return invalidate("duplicate reconciliation orders completion");
    if (open_orders != 0) return invalidate("reconciliation has open orders");
    orders_finished_ = true;
    return true;
}

bool AccountReconciliation::finish_snapshot(std::uint64_t epoch, std::uint64_t token) {
    if (!current_snapshot(epoch, token)) return false;
    if (!account_seen_ || !positions_finished_ || !orders_finished_)
        return invalidate("reconciliation snapshot lacks successful query completions");
    snapshot_active_ = false;
    snapshot_finished_ = true;
    failure_reason_.clear();
    last_rejection_.clear();
    return true;
}

bool AccountReconciliation::on_activity(std::uint64_t epoch) {
    if (!current_epoch(epoch)) return false;
    if (snapshot_active_) return invalidate("account activity during reconciliation snapshot");
    return true;
}

void AccountReconciliation::begin_stop() {
    stopped_ = true;
    connected_ = false;
    invalidate("reconciliation stopped");
}

bool AccountReconciliation::ready() const {
    return !stopped_ && connected_ && epoch_ != 0 && snapshot_token_ != 0 &&
           snapshot_token_ == latest_token_ && !snapshot_invalid_ && snapshot_finished_ &&
           account_seen_ && positions_finished_ && orders_finished_ &&
           positions_.size() == instruments_.size();
}

const AccountReconciliation::Positions& AccountReconciliation::positions() const {
    if (!ready()) throw std::logic_error("reconciliation positions unavailable before readiness");
    return positions_;
}

double AccountReconciliation::available_cash() const {
    if (!ready()) throw std::logic_error("reconciliation cash unavailable before readiness");
    return available_cash_;
}

}  // namespace strategy_runtime
