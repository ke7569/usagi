#include "sse/auction/auction59_session.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

std::uint64_t at(int hour, int minute, int second) {
    return static_cast<std::uint64_t>(hour * 3600 + minute * 60 + second) * 1000000ULL;
}

sse_auction59::StaticMetadata metadata() {
    sse_auction59::StaticMetadata result;
    result.date = 20260909;
    result.pre_close = 10.0;
    result.lower_limit = 9.0;
    result.upper_limit = 11.0;
    result.limits_valid = true;
    return result;
}

sse_live::TickEvent order(char type, std::uint64_t seq, std::uint64_t time,
                          bool buy, std::uint64_t id, std::uint32_t price,
                          std::uint64_t quantity) {
    sse_live::TickEvent event = {};
    event.security_id = "600001";
    event.channel_no = 1;
    event.app_seq_num = seq;
    event.time_of_day_micros = time;
    event.event_type = type;
    event.buy_order_no = buy ? id : 0;
    event.sell_order_no = buy ? 0 : id;
    event.price_raw = price;
    event.quantity_raw = quantity * 1000ULL;
    event.side = buy ? 0 : 1;
    return event;
}

sse_live::TickEvent trade(std::uint64_t seq, std::uint64_t buy, std::uint64_t sell,
                          std::uint64_t quantity) {
    sse_live::TickEvent event = order('T', seq, at(9, 25, 0), true, buy, 10000, quantity);
    event.sell_order_no = sell;
    return event;
}

sse_live::TickEvent status() {
    sse_live::TickEvent event = order('S', 18, at(9, 25, 0), true, 0, 0, 0);
    event.side = 3;
    return event;
}

std::vector<sse_live::TickEvent> fixture() {
    // The engine's original conservation fixture, with order 3 moved to
    // 09:24. This gives a non-flat price path and a marketable last-minute
    // order, making all 59 projected factors defined without filling NaNs.
    const sse_live::TickEvent events[] = {
        order('A', 1, at(9,15,0), true, 1, 10000, 80),
        order('A', 2, at(9,15,0), false, 2, 9980, 70),
        order('A', 3, at(9,17,0), false, 4, 9990, 90),
        order('A', 4, at(9,18,0), true, 5, 10000, 50),
        order('A', 5, at(9,18,0), false, 6, 9990, 60),
        order('D', 6, at(9,18,4), true, 5, 10000, 50),
        order('D', 7, at(9,18,4), false, 6, 9990, 60),
        order('A', 8, at(9,19,0), false, 7, 10000, 80),
        order('A', 9, at(9,20,0), false, 8, 10010, 40),
        order('A', 10, at(9,21,0), true, 9, 10000, 50),
        order('A', 11, at(9,24,0), true, 3, 10020, 120),
        order('A', 12, at(9,24,0), true, 10, 9990, 30),
        trade(13, 1, 2, 70), trade(14, 1, 4, 10), trade(15, 3, 4, 80),
        trade(16, 3, 7, 40), trade(17, 9, 7, 40)
    };
    return std::vector<sse_live::TickEvent>(events, events + sizeof(events) / sizeof(events[0]));
}

void feed(sse_auction59::Session* session, bool with_status = true) {
    const std::vector<sse_live::TickEvent> events = fixture();
    for (std::size_t i = 0; i < events.size(); ++i) session->on_tick(events[i], 0, i + 1U);
    if (with_status) session->on_tick(status(), 0, 18);
}

sse_live::Snapshot snapshot(std::uint64_t time = at(9, 25, 1)) {
    sse_live::Snapshot result = {};
    result.security_id = "600001";
    result.time_of_day_micros = time;
    result.pre_close_price = 10.0;
    result.open_price = 10.0;
    result.volume = 240;
    return result;
}

}  // namespace

int main() {
    const std::vector<sse_live::TickEvent> events = fixture();
    sse_auction59::AuctionAccumulator reference("600001", metadata());
    for (std::size_t i = 0; i < events.size(); ++i) reference.observe(events[i], 0, i + 1U);
    reference.observe(status(), 0, 18);
    const sse_auction59::AuctionResult expected = reference.finalize();
    require(expected.hard_valid, "fixture does not conserve orders/opening match");
    require(expected.projected_valid_mask == (1ULL << 59U) - 1ULL,
            "fixture does not define all 59 factors");

    sse_auction59::Session normal("600001", metadata());
    feed(&normal);
    require(!normal.ready() && !normal.failed(), "status alone must await snapshot reconciliation");
    normal.on_snapshot(snapshot());
    require(normal.ready() && normal.shared_static_valid(), "matching opening snapshot did not become ready");
    require(normal.factors().size() == 59U, "wrong factor count");
    for (std::size_t i = 0; i < 59U; ++i)
        require(std::isfinite(normal.factors()[i]) && normal.factors()[i] == expected.factors[i],
                "in-memory factors differ from existing engine");
    const std::vector<float> frozen = normal.factors();
    normal.on_tick(status(), 0, 19);
    normal.seal_missing();
    require(normal.ready() && normal.factors() == frozen, "ready factors changed after opening freeze");

    sse_auction59::Session snapshot_first("600001", metadata());
    snapshot_first.on_snapshot(snapshot());
    feed(&snapshot_first);
    require(snapshot_first.ready() && snapshot_first.factors() == frozen,
            "snapshot/status arrival order changed result");

    sse_auction59::Session early("600001", metadata());
    sse_live::Snapshot incomplete = snapshot(at(9,25,0));
    incomplete.volume = 0;
    incomplete.open_price = 0;
    early.on_snapshot(incomplete);
    feed(&early);
    require(!early.ready() && !early.failed(), "early 09:25 snapshot should await opening update");
    early.on_snapshot(snapshot());
    require(early.ready(), "later complete opening snapshot did not unblock readiness");

    sse_auction59::Session after_open("600001", metadata());
    feed(&after_open);
    sse_live::Snapshot continuous = snapshot(at(9,30,0));
    continuous.volume = 1000000;
    after_open.on_snapshot(continuous);
    require(after_open.ready(), "09:30 cumulative volume was compared to auction-only volume");

    sse_auction59::Session no_status("600001", metadata());
    feed(&no_status, false);
    no_status.on_snapshot(snapshot());
    no_status.seal_missing();
    no_status.on_tick(status(), 0, 18);
    require(no_status.failed() && !no_status.ready() && no_status.shared_static_valid(),
            "missing S must remain local tick-only after sealing");

    sse_auction59::Session no_snapshot("600001", metadata());
    feed(&no_snapshot);
    no_snapshot.seal_missing();
    no_snapshot.on_snapshot(snapshot());
    require(no_snapshot.failed() && !no_snapshot.ready(), "late snapshot hot-started sealed session");

    sse_auction59::Session bad_boundary("600001", metadata());
    feed(&bad_boundary, false);
    sse_live::TickEvent malformed_status = status();
    malformed_status.side = 0;
    bad_boundary.on_tick(malformed_status, 0, 18);
    require(bad_boundary.failed() && bad_boundary.shared_static_valid(), "malformed S was accepted");

    sse_auction59::Session bad_volume("600001", metadata());
    feed(&bad_volume);
    sse_live::Snapshot incorrect = snapshot();
    incorrect.volume = 240000;
    bad_volume.on_snapshot(incorrect);
    require(bad_volume.failed() && bad_volume.reason() == "snapshot_auction_volume_conflict",
            "snapshot volume unit mismatch went unnoticed");

    sse_auction59::Session bad_raw_units("600001", metadata());
    for (std::size_t i = 0; i < events.size(); ++i) {
        sse_live::TickEvent event = events[i];
        if (event.event_type == 'T') event.quantity_raw /= 1000ULL;
        bad_raw_units.on_tick(event, 0, i + 1U);
    }
    bad_raw_units.on_tick(status(), 0, 18);
    bad_raw_units.on_snapshot(snapshot());
    require(bad_raw_units.failed() && bad_raw_units.shared_static_valid(),
            "canonical shares accidentally supplied as raw tick quantity were accepted");

    sse_auction59::Session bad_open("600001", metadata());
    feed(&bad_open);
    incorrect = snapshot();
    incorrect.open_price = 10.01;
    bad_open.on_snapshot(incorrect);
    require(bad_open.failed() && bad_open.shared_static_valid(), "opening-price mismatch was accepted");

    incorrect = snapshot(at(9,30,3));
    incorrect.pre_close_price = 9.99;
    normal.on_snapshot(incorrect);
    require(normal.failed() && !normal.shared_static_valid() && normal.factors().empty(),
            "post-ready static conflict must block tick trading too");
    require(snapshot_first.ready(), "one security/session failure leaked to another");

    sse_auction59::Session bad_limits("600001", metadata());
    incorrect = snapshot();
    incorrect.open_price = 12.0;
    bad_limits.on_snapshot(incorrect);
    require(!bad_limits.shared_static_valid(), "open outside daily limits was only an auction failure");

    sse_auction59::StaticMetadata invalid = metadata();
    invalid.pre_close = std::numeric_limits<double>::quiet_NaN();
    sse_auction59::Session invalid_static("600001", invalid);
    require(invalid_static.failed() && !invalid_static.shared_static_valid(), "nonfinite static price accepted");

    sse_auction59::StaticMetadata ipo = metadata();
    ipo.lower_limit = 6.4;
    ipo.upper_limit = 14.4;
    sse_auction59::Session unknown_ipo("600001", ipo);
    require(unknown_ipo.failed() && unknown_ipo.shared_static_valid() &&
            unknown_ipo.reason() == "ipo_first_day_unknown", "unknown special IPO case was guessed");
    ipo.is_ipo_first_day = true;
    sse_auction59::Session known_ipo("600001", ipo, true);
    feed(&known_ipo);
    known_ipo.on_snapshot(snapshot());
    require(known_ipo.ready(), "explicit IPO flag did not preserve existing engine special case");

    // Flat-price paths can conserve all orders yet contain undefined model
    // factors. Do not equate the engine's hard_valid with model readiness.
    sse_auction59::Session flat("600001", metadata());
    std::vector<sse_live::TickEvent> flat_events = events;
    sse_live::TickEvent moved = flat_events[10];
    flat_events.erase(flat_events.begin() + 10);
    moved.time_of_day_micros = at(9,17,0);
    flat_events.insert(flat_events.begin() + 2, moved);
    for (std::size_t i = 0; i < flat_events.size(); ++i) {
        flat_events[i].app_seq_num = i + 1U;
        flat.on_tick(flat_events[i], 0, i + 1U);
    }
    flat.on_tick(status(), 0, 18);
    flat.on_snapshot(snapshot());
    require(flat.failed() && flat.reason() == "auction_factors_incomplete",
            "undefined Auction59 factors were silently substituted");

    std::cout << "auction59_session_test: ok\n";
    return 0;
}
