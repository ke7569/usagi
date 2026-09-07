#ifndef T0_ACCOUNT_RECONCILIATION_H
#define T0_ACCOUNT_RECONCILIATION_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace strategy_runtime {

struct AccountIdentity {
    std::string account;
    short source;
    std::uint32_t day;
};

struct PositionSnapshot {
    int total;
    int available;
};

// The owner serializes every call, including connection and activity events.
// Readiness requires a quiescent snapshot: adapters must report intervening
// account/order activity and explicit successful query-completion markers.
// A missing SDK completion marker must leave the gate closed.
class AccountReconciliation {
public:
    typedef std::map<std::string, PositionSnapshot> Positions;

    AccountReconciliation(const AccountIdentity& identity,
                          const std::set<std::string>& instruments);
    bool begin_epoch(std::uint64_t epoch);
    bool mark_connected(std::uint64_t epoch, bool connected);
    bool begin_snapshot(std::uint64_t epoch, std::uint64_t snapshot_token);
    bool add_account(std::uint64_t epoch, std::uint64_t snapshot_token,
                     const AccountIdentity& identity, double available_cash);
    bool add_position(std::uint64_t epoch, std::uint64_t snapshot_token,
                      const AccountIdentity& identity, const std::string& instrument,
                      int total, int available);
    bool finish_positions(std::uint64_t epoch, std::uint64_t snapshot_token);
    bool finish_orders(std::uint64_t epoch, std::uint64_t snapshot_token,
                       std::size_t open_orders);
    bool finish_snapshot(std::uint64_t epoch, std::uint64_t snapshot_token);
    bool on_activity(std::uint64_t epoch);
    void begin_stop();

    bool ready() const;
    const Positions& positions() const;
    double available_cash() const;
    const AccountIdentity& identity() const { return identity_; }
    const std::string& failure_reason() const { return failure_reason_; }
    // Rejected stale events are observable without changing the current snapshot.
    const std::string& last_rejection() const { return last_rejection_; }

private:
    bool reject(const std::string& reason);
    bool invalidate(const std::string& reason);
    bool current_epoch(std::uint64_t epoch);
    bool current_snapshot(std::uint64_t epoch, std::uint64_t snapshot_token);
    bool same_identity(const AccountIdentity& identity) const;
    void clear_snapshot();

    AccountIdentity identity_;
    std::set<std::string> instruments_;
    Positions positions_;
    std::uint64_t epoch_, snapshot_token_, latest_token_;
    bool connected_, stopped_, snapshot_active_, snapshot_invalid_;
    bool account_seen_, positions_finished_, orders_finished_, snapshot_finished_;
    double available_cash_;
    std::string failure_reason_, last_rejection_;
};

}  // namespace strategy_runtime
#endif
