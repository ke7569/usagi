#include "common/execution/AtpSnapshotCollector.h"
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace strategy_runtime;
using namespace oms;
static void ok(bool x, const char* m) { if (!x) throw std::runtime_error(m); }
static Scope scope() {
    Scope s; s.account.account="a"; s.account.broker="b"; s.gateway="g";
    s.day=20260909; s.epoch=1; s.source=190; return s;
}
static Instrument stock(const std::string& code = "600000") {
    Instrument i; i.market="SSE"; i.code=code; return i;
}
static SnapshotPosition pos() {
    SnapshotPosition p; p.instrument=stock(); p.total=200; p.free_sellable=200; return p;
}
static SnapshotOrder order(const std::string& id = "br1") {
    SnapshotOrder o; o.id=0; o.broker_id=id; o.instrument=stock(); o.side=Side::Buy;
    o.price=10000; o.original=100; o.filled=100; o.working=0; o.state=OrderState::Filled; return o;
}
static Report trade(const Scope& s) {
    Report r; r.scope=s; r.broker_id="br1"; r.instrument=stock(); r.side=Side::Buy;
    r.kind=ReportKind::Trade; r.trade_id="t1"; r.trade_quantity=100; r.trade_price=10000; return r;
}
static void empty_end(AtpSnapshotCollector& c, const Scope& s, std::uint64_t token,
                      AtpSnapshotCollector::Stream stream, std::uint64_t cursor = 0) {
    const auto page = c.finish_page(s, token, stream, cursor, cursor, 0, 2);
    ok(page.accepted && page.done, "empty terminal query page");
}
static void funds_positions(AtpSnapshotCollector& c, const Scope& s, std::uint64_t token) {
    ok(c.add_funds(s, token, 1000000), "funds row");
    ok(c.finish_funds(s, token), "funds end");
    ok(c.finish_all(s, token, AtpSnapshotCollector::Positions), "empty positions explicit all-results end");
}
static void test_empty_positions_and_incomplete_snapshot() {
    const Scope s = scope(); std::set<Instrument> u; u.insert(stock());
    AtpSnapshotCollector c; ok(c.begin(s, 7, u), "begin");
    funds_positions(c, s, 7);
    empty_end(c, s, 7, AtpSnapshotCollector::Orders);
    ok(!c.ready() && !c.snapshot().account_success && c.snapshot().positions.empty(),
       "missing trades cannot certify snapshot or manufacture zero holdings yet");
    empty_end(c, s, 7, AtpSnapshotCollector::Trades);
    const Snapshot& snapshot = c.snapshot();
    ok(c.ready() && snapshot.positions.size() == 1 && snapshot.positions[0].total == 0 &&
       snapshot.positions[0].free_sellable == 0 && snapshot.positions[0].instrument == stock(),
       "complete empty query must cover strategy universe with zero holding");
    ok(snapshot.account_success && snapshot.positions_success && snapshot.orders_success &&
       snapshot.trades_success && snapshot.all_day_orders && snapshot.all_day_trades,
       "four successful queries certify complete all-day snapshot");
}
static void test_pagination_and_cross_page_dedup() {
    const Scope s = scope(); AtpSnapshotCollector c;
    ok(c.begin(s, 1, std::set<Instrument>()), "paged begin"); funds_positions(c, s, 1);
    const SnapshotOrder first = order(), second = order("br2");
    ok(c.add_order(s, 1, first) && c.add_order(s, 1, second), "first order page");
    auto page = c.finish_page(s, 1, AtpSnapshotCollector::Orders, 0, 10, 2, 2);
    ok(page.accepted && !page.done && page.next_index == 10, "full page requires next cursor");
    ok(c.add_order(s, 1, second), "duplicate order at next page boundary");
    page = c.finish_page(s, 1, AtpSnapshotCollector::Orders, 10, 11, 1, 2);
    ok(page.accepted && page.done, "short page finishes query using raw rows count");
    const Report row = trade(s);
    ok(c.add_trade(s, 1, row), "trade page 1");
    page = c.finish_page(s, 1, AtpSnapshotCollector::Trades, 0, 20, 1, 1);
    ok(page.accepted && !page.done && !c.ready(), "last callback of a full page is not query completion");
    ok(c.add_trade(s, 1, row), "duplicate trade page 2");
    page = c.finish_page(s, 1, AtpSnapshotCollector::Trades, 20, 21, 1, 1);
    ok(page.accepted && !page.done, "duplicate rows still consume pagination");
    page = c.finish_page(s, 1, AtpSnapshotCollector::Trades, 21, 21, 0, 1);
    ok(page.accepted && page.done && c.ready(), "empty page confirms full-page boundary completion");
    ok(c.snapshot().orders.size() == 2 && c.snapshot().trades.size() == 1,
       "cross-page facts deduplicate");
    ok(c.snapshot().positions.size() == 1 && c.snapshot().positions[0].total == 0,
       "historical order outside universe gets proven zero position");
}
static void test_stale_generation_isolation_and_restart() {
    Scope s = scope(); const Scope old = s; ++s.epoch;
    std::set<Instrument> u; u.insert(stock()); AtpSnapshotCollector c;
    ok(c.begin(old, 5, u) && c.begin(s, 1, u), "new epoch allows reset token");
    ok(!c.add_funds(old, 5, 3) && !c.add_position(old, 5, pos()), "stale epoch rejected");
    ok(!c.begin(old, 100, u) && !c.begin(s, 1, u), "stale begin cannot reset active snapshot");
    ok(!c.add_funds(s, 0, 4) && !c.fail(old, 5, "old error"), "stale token/error rejected");
    c.activity(old); ok(c.error().empty(), "old activity does not invalidate current epoch");
    funds_positions(c, s, 1); empty_end(c, s, 1, AtpSnapshotCollector::Orders);
    empty_end(c, s, 1, AtpSnapshotCollector::Trades);
    ok(c.ready() && c.snapshot().scope == s && c.snapshot().free_cash == 1000000,
       "current snapshot survives stale callbacks");
    ok(!c.add_funds(old, 5, 0) && c.ready(), "stale callback cannot close completed current snapshot");
    ok(c.begin(s, 2, u) && !c.ready() && c.snapshot().positions.empty(), "new query clears all old facts/readiness");
}
static void test_errors_and_activity_never_certify() {
    const Scope s = scope(); AtpSnapshotCollector c;
    ok(c.begin(s, 1, std::set<Instrument>()), "failure begin");
    ok(!c.finish_funds(s, 1) && !c.error().empty(), "empty funds callback is not zero cash");
    ok(!c.add_funds(s, 1, 1) && !c.ready(), "failed snapshot cannot recover in same token");
    ok(c.begin(s, 2, std::set<Instrument>()), "activity begin");
    funds_positions(c, s, 2); c.activity(s);
    ok(!c.finish_page(s, 2, AtpSnapshotCollector::Orders, 0, 0, 0, 2).accepted &&
       !c.ready() && !c.snapshot().positions_success, "activity invalidates completed streams");
    ok(c.begin(s, 3, std::set<Instrument>()), "query failure begin");
    funds_positions(c, s, 3); ok(!c.fail(s, 3, "broker returned error 7"), "query error rejected");
    ok(c.error() == "broker returned error 7" && !c.ready(), "query error preserved");
    ok(c.begin(s, 4, std::set<Instrument>()), "fresh query after activity");
    funds_positions(c, s, 4); empty_end(c, s, 4, AtpSnapshotCollector::Orders);
    empty_end(c, s, 4, AtpSnapshotCollector::Trades); ok(c.ready(), "new token may certify");
    c.activity(s); ok(!c.ready() && !c.snapshot().all_day_orders, "activity invalidates an undelivered completed snapshot");
}
static void test_conflicts_and_malformed_pages() {
    const Scope s = scope(); AtpSnapshotCollector c;
    ok(c.begin(s, 1, std::set<Instrument>()), "conflict begin");
    ok(c.add_order(s, 1, order()), "first order");
    SnapshotOrder changed = order(); ++changed.price;
    ok(!c.add_order(s, 1, changed) && !c.ready(), "conflicting duplicate order cannot be silently deduplicated");
    ok(c.begin(s, 2, std::set<Instrument>()), "trade conflict begin");
    Report row = trade(s); ok(c.add_trade(s, 2, row), "first trade"); ++row.trade_fee;
    ok(!c.add_trade(s, 2, row), "conflicting trade fee invalidates");
    ok(c.begin(s, 3, std::set<Instrument>()), "page begin");
    ok(c.add_order(s, 3, order()), "page order");
    ok(!c.finish_page(s, 3, AtpSnapshotCollector::Orders, 0, 0, 1, 1).accepted,
       "full page with non-progressing cursor invalidates");
    ok(c.begin(s, 4, std::set<Instrument>()), "mismatched count begin");
    ok(c.add_order(s, 4, order()), "count order");
    ok(!c.finish_page(s, 4, AtpSnapshotCollector::Orders, 0, 0, 0, 2).accepted,
       "page raw row count must match callbacks");
    ok(c.begin(s, 5, std::set<Instrument>()), "mismatched cursor begin");
    ok(!c.finish_page(s, 5, AtpSnapshotCollector::Orders, 7, 7, 0, 2).accepted,
       "unexpected page start cursor cannot certify empty result");
}
static void test_filtered_records_do_not_truncate_page() {
    const Scope s = scope(); AtpSnapshotCollector c;
    ok(c.begin(s, 1, std::set<Instrument>()), "filtered begin"); funds_positions(c, s, 1);
    ok(c.add_order(s, 1, order()), "original order row");
    ok(c.skip_row(s, 1, AtpSnapshotCollector::Orders), "cancel-request record counted");
    const auto page = c.finish_page(s, 1, AtpSnapshotCollector::Orders, 0, 15, 2, 2);
    ok(page.accepted && !page.done && page.next_index == 15,
       "filtered record in full page must still request next page");
    empty_end(c, s, 1, AtpSnapshotCollector::Orders, 15);
    ok(c.skip_row(s, 1, AtpSnapshotCollector::Trades), "non-trade record counted");
    ok(c.finish_page(s, 1, AtpSnapshotCollector::Trades, 0, 17, 1, 2).done, "short filtered trade page");
    ok(c.ready() && c.snapshot().orders.size() == 1 && c.snapshot().trades.empty(),
       "filtered rows do not become fictitious orders or trades");
}

int main() {
  try {
    test_empty_positions_and_incomplete_snapshot();
    test_pagination_and_cross_page_dedup();
    test_stale_generation_isolation_and_restart();
    test_errors_and_activity_never_certify();
    test_conflicts_and_malformed_pages();
    test_filtered_records_do_not_truncate_page();
  } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return EXIT_FAILURE; }
  return EXIT_SUCCESS;
}
