#ifndef USAGI_ATP_SNAPSHOT_COLLECTOR_H
#define USAGI_ATP_SNAPSHOT_COLLECTOR_H

#include "common/oms/Types.h"
#include <map>
#include <set>
#include <string>
#include <utility>

namespace strategy_runtime {

// The adapter serializes calls, including connection and unsolicited activity
// events. All quantities/money must already use the canonical OMS units.
class AtpSnapshotCollector {
public:
    enum Stream { Positions, Orders, Trades };
    struct PageResult {
        bool accepted;
        bool done;
        std::uint64_t next_index;
        PageResult(bool a = false, bool d = false, std::uint64_t n = 0)
            : accepted(a), done(d), next_index(n) {}
    };

    AtpSnapshotCollector()
        : initialized_(false), active_(false), invalid_(false), ready_(false),
          funds_seen_(false), funds_done_(false) {}

    bool begin(const oms::Scope& scope, std::uint64_t token,
               const std::set<oms::Instrument>& universe) {
        if (!token || !scope.epoch) return reject("zero snapshot token or epoch");
        if (initialized_ && (!(scope.account == snapshot_.scope.account) ||
            scope.gateway != snapshot_.scope.gateway || scope.source != snapshot_.scope.source ||
            scope.day < snapshot_.scope.day ||
            (scope.day == snapshot_.scope.day && scope.epoch < snapshot_.scope.epoch) ||
            (scope == snapshot_.scope && token <= snapshot_.token)))
            return reject("stale or foreign snapshot begin");
        snapshot_ = oms::Snapshot(); snapshot_.scope = scope; snapshot_.token = token;
        universe_ = universe;
        positions_.clear(); orders_.clear(); trades_.clear();
        for (int i = 0; i < 3; ++i) pages_[i] = Page();
        error_.clear(); rejection_.clear();
        initialized_ = active_ = true;
        invalid_ = ready_ = funds_seen_ = funds_done_ = false;
        return true;
    }

    bool add_funds(const oms::Scope& scope, std::uint64_t token, oms::Money money) {
        if (!current(scope, token)) return false;
        if (funds_done_) return invalidate("funds row after completion");
        if (money < 0) return invalidate("negative free cash");
        if (funds_seen_ && snapshot_.free_cash != money)
            return invalidate("conflicting duplicate funds");
        snapshot_.free_cash = money; funds_seen_ = true; return true;
    }
    bool add_position(const oms::Scope& scope, std::uint64_t token,
                      const oms::SnapshotPosition& row) {
        if (!row_allowed(scope, token, Positions)) return false;
        if (!instrument_ok(row.instrument) || row.total < 0 || row.free_sellable < 0 ||
            row.free_sellable > row.total) return invalidate("invalid snapshot position");
        ++pages_[Positions].rows;
        const auto existing = positions_.find(row.instrument);
        if (existing == positions_.end()) positions_.insert(std::make_pair(row.instrument, row));
        else if (existing->second.total != row.total ||
                 existing->second.free_sellable != row.free_sellable)
            return invalidate("conflicting duplicate position");
        return true;
    }
    bool add_order(const oms::Scope& scope, std::uint64_t token,
                   const oms::SnapshotOrder& row) {
        if (!row_allowed(scope, token, Orders)) return false;
        if (!instrument_ok(row.instrument) || row.broker_id.empty() || row.price <= 0 ||
            row.original <= 0 || row.filled < 0 || row.working < 0 ||
            row.filled > row.original || row.working > row.original - row.filled ||
            !side_ok(row.side)) return invalidate("invalid snapshot order");
        ++pages_[Orders].rows;
        const auto existing = orders_.find(row.broker_id);
        if (existing == orders_.end()) orders_.insert(std::make_pair(row.broker_id, row));
        else if (!same_order(existing->second, row))
            return invalidate("conflicting duplicate order");
        return true;
    }
    bool add_trade(const oms::Scope& scope, std::uint64_t token,
                   const oms::Report& row) {
        if (!(row.scope == scope)) return reject("foreign trade row scope");
        if (!row_allowed(scope, token, Trades)) return false;
        if (!instrument_ok(row.instrument) || row.kind != oms::ReportKind::Trade ||
            row.trade_id.empty() || row.broker_id.empty() || row.trade_quantity <= 0 ||
            row.trade_price < 0 || row.trade_fee < -1 || !side_ok(row.side) || row.error.failed())
            return invalidate("invalid snapshot trade");
        ++pages_[Trades].rows;
        const TradeKey key(row.broker_id, row.trade_id);
        const auto existing = trades_.find(key);
        if (existing == trades_.end()) trades_.insert(std::make_pair(key, row));
        else if (!same_trade(existing->second, row))
            return invalidate("conflicting duplicate trade");
        return true;
    }

    bool finish_funds(const oms::Scope& scope, std::uint64_t token) {
        if (!current(scope, token)) return false;
        if (!funds_seen_ || funds_done_) return invalidate("missing funds or duplicate completion");
        funds_done_ = true; maybe_complete(); return true;
    }
    // Count SDK query records that have no canonical row (e.g. cancel requests).
    // Pagination uses raw records, never the deduplicated snapshot vector size.
    bool skip_row(const oms::Scope& scope, std::uint64_t token, Stream stream) {
        if (!row_allowed(scope, token, stream)) return false;
        ++pages_[stream].rows; return true;
    }
    // Use only for an SDK query that explicitly returns the whole result set,
    // such as the ATP holding query with ReturnNum=0. isLast alone is not proof
    // that paginated order/trade queries are complete.
    bool finish_all(const oms::Scope& scope, std::uint64_t token, Stream stream) {
        if (!row_allowed(scope, token, stream)) return false;
        if (stream != Positions) return invalidate("all-results completion requires position query");
        pages_[stream].done = true; maybe_complete(); return true;
    }
    PageResult finish_page(const oms::Scope& scope, std::uint64_t token, Stream stream,
                           std::uint64_t request_index, std::uint64_t last_index,
                           std::size_t rows_count, std::size_t page_size) {
        if (!row_allowed(scope, token, stream)) return PageResult();
        Page& page = pages_[stream];
        if (!page_size || rows_count > page_size || rows_count != page.rows ||
            request_index != page.next_index ||
            (rows_count == page_size && last_index <= request_index)) {
            invalidate("invalid or non-progressing query page"); return PageResult();
        }
        page.done = rows_count < page_size;
        page.next_index = page.done ? 0 : last_index;
        page.rows = 0;
        maybe_complete();
        return PageResult(true, page.done, page.next_index);
    }
    bool fail(const oms::Scope& scope, std::uint64_t token, const std::string& why) {
        if (!current(scope, token)) return false;
        return invalidate(why.empty() ? "snapshot query failed" : why);
    }
    void activity(const oms::Scope& scope) {
        if (!initialized_ || !(scope == snapshot_.scope)) {
            reject("stale activity scope"); return;
        }
        if (active_ || ready_) invalidate("account activity during snapshot");
    }
    bool ready() const { return ready_ && !invalid_; }
    const oms::Snapshot& snapshot() const { return snapshot_; }
    const std::string& error() const { return error_; }
    const std::string& last_rejection() const { return rejection_; }

private:
    struct Page {
        bool done;
        std::size_t rows;
        std::uint64_t next_index;
        Page() : done(false), rows(0), next_index(0) {}
    };
    typedef std::pair<std::string, std::string> TradeKey;
    static bool instrument_ok(const oms::Instrument& i) {
        return !i.market.empty() && !i.code.empty();
    }
    static bool side_ok(oms::Side s) { return s == oms::Side::Buy || s == oms::Side::Sell; }
    static bool same_order(const oms::SnapshotOrder& a, const oms::SnapshotOrder& b) {
        return a.id == b.id && a.owner == b.owner && a.broker_id == b.broker_id &&
            a.instrument == b.instrument && a.side == b.side && a.price == b.price &&
            a.original == b.original && a.filled == b.filled && a.working == b.working &&
            a.state == b.state;
    }
    static bool same_trade(const oms::Report& a, const oms::Report& b) {
        return a.scope == b.scope && a.id == b.id && a.broker_id == b.broker_id &&
            a.instrument == b.instrument && a.side == b.side && a.kind == b.kind &&
            a.trade_id == b.trade_id && a.trade_quantity == b.trade_quantity &&
            a.trade_price == b.trade_price && a.trade_fee == b.trade_fee &&
            a.cumulative_after == b.cumulative_after && a.fee_is_final == b.fee_is_final;
    }
    bool reject(const std::string& why) { rejection_ = why; return false; }
    bool current(const oms::Scope& scope, std::uint64_t token) {
        if (!initialized_ || !(scope == snapshot_.scope) || token != snapshot_.token)
            return reject("stale snapshot scope or token");
        if (!active_ || invalid_) return reject("snapshot is closed or failed");
        return true;
    }
    bool row_allowed(const oms::Scope& scope, std::uint64_t token, Stream stream) {
        if (!current(scope, token)) return false;
        if (stream < Positions || stream > Trades) return invalidate("unknown snapshot stream");
        if (pages_[stream].done) return invalidate("query row or completion after stream ended");
        return true;
    }
    bool invalidate(const std::string& why) {
        invalid_ = true; ready_ = false; error_ = why;
        snapshot_.account_success = snapshot_.positions_success = false;
        snapshot_.orders_success = snapshot_.trades_success = false;
        snapshot_.all_day_orders = snapshot_.all_day_trades = false;
        return false;
    }
    void maybe_complete() {
        if (invalid_ || !funds_done_ || !pages_[Positions].done ||
            !pages_[Orders].done || !pages_[Trades].done) return;
        // A completed all-position query proves an absent holding is zero.
        // Include historical order instruments as well as the strategy universe.
        std::set<oms::Instrument> coverage = universe_;
        for (const auto& item : orders_) coverage.insert(item.second.instrument);
        for (const auto& instrument : coverage) {
            if (!positions_.count(instrument)) {
                oms::SnapshotPosition row; row.instrument = instrument;
                positions_.insert(std::make_pair(instrument, row));
            }
        }
        for (const auto& item : positions_) snapshot_.positions.push_back(item.second);
        for (const auto& item : orders_) snapshot_.orders.push_back(item.second);
        for (const auto& item : trades_) snapshot_.trades.push_back(item.second);
        snapshot_.account_success = snapshot_.positions_success = true;
        snapshot_.orders_success = snapshot_.trades_success = true;
        snapshot_.all_day_orders = snapshot_.all_day_trades = true;
        ready_ = true; active_ = false;
    }

    oms::Snapshot snapshot_;
    std::set<oms::Instrument> universe_;
    std::map<oms::Instrument, oms::SnapshotPosition> positions_;
    std::map<std::string, oms::SnapshotOrder> orders_;
    std::map<TradeKey, oms::Report> trades_;
    Page pages_[3];
    bool initialized_, active_, invalid_, ready_, funds_seen_, funds_done_;
    std::string error_, rejection_;
};

}  // namespace strategy_runtime
#endif
