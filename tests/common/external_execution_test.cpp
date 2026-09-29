#include "common/strategy/StrategySession.h"
#include "common/oms/Records.h"
#include "tests/oms/TestExecution.h"
#include "common/contracts/legacy/LFConstants.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }

struct Fixture {
    std::string market, code, exchange;
    nlohmann::json config;
    std::unique_ptr<oms_test::ManagedFixture> oms;
    std::unique_ptr<strategy_runtime::StrategySession> session;
    bool healthy = true;
    std::vector<oms::Command> submitted;

    Fixture(const std::string& m, int delta)
        : market(m), code(m == "SH" ? "600000" : "000001"), exchange(m == "SH" ? "SSE" : "SZE") {
        config["market"] = market;
        config["global_params"] = {{"offset", 1.0}, {"global_bias_factor", 1.0},
            {"position_base_line", 100000.0}, {"position_limit", 1.0}};
        config["sze_startup_warmup_signals"] = 0;
        const std::string symbol = code + (market == "SH" ? ".SH" : ".SZ");
        config["ins_params"][symbol] = {{"static_position", 1000 + delta},
            {"last_position", 0}, {"external_delta", delta}};
        auto physical = config;
        physical["ins_params"][symbol]["static_position"] = 1000;
        oms.reset(new oms_test::ManagedFixture(physical, market, 88));
        oms->backend->submit_hook = [this](const oms::Command& c) {
            submitted.push_back(c);
            oms::SendResult result; result.disposition = oms::SendDisposition::Submitted;
            result.broker_id = "B" + std::to_string(c.id); return result;
        };
        session.reset(new strategy_runtime::StrategySession(config, market, 88, oms->execution,
            [this]() { return healthy; }));
        session->set_ready(true, true, true);
    }

    MSMarketDataField book(int hhmm = 930) const {
        MSMarketDataField v = {MSMarketData()};
        v.MarketTime = hhmm * 100000.0;
        double* values = v.ms_market_data.ms_market_data.data();
        for (int i = 0; i < 3; ++i) {
            values[BidPrice1Index + i] = 10.0 - .01 * i;
            values[AskPrice1Index + i] = 10.01 + .01 * i;
            values[BidVolume1Index + i] = values[AskVolume1Index + i] = 100;
        }
        v.LastPrice = v.MidPrice = 10.005;
        return v;
    }
    int round(int hhmm, int t0 = 0) {
        int id = -1;
        const auto v = book(hhmm);
        session->on_decision(code, v, [&]() {
            if (t0) id = session->execution()->submit_limit_then_cancel(88, code, exchange,
                t0 > 0 ? 10.01 : 10.0, std::abs(t0), t0 > 0 ? LF_CHAR_Buy : LF_CHAR_Sell,
                t0 > 0 ? LF_CHAR_Open : LF_CHAR_Close, 1001, "t0-test");
        });
        return id;
    }
    std::vector<oms::Command> commands() const {
        return submitted;
    }
    oms::Report report(const oms::Command& c, oms::OrderState state, int filled) const {
        oms::Report r;
        r.scope = oms->engine->scope(); r.id = c.id; r.broker_id = "B" + std::to_string(c.id);
        r.instrument = c.intent.instrument; r.side = c.intent.side;
        r.state = state; r.original = c.intent.quantity; r.cumulative = filled;
        r.leaves = c.intent.quantity - filled;
        return r;
    }
    void fill(const oms::Command& c, int quantity, int cumulative, const std::string& id, double price = 10.01) {
        auto r = report(c, oms::OrderState::Partial, cumulative);
        r.kind = oms::ReportKind::Trade; r.trade_id = id;
        r.trade_quantity = quantity; r.cumulative_after = cumulative;
        check(oms::money_from_double(price, &r.trade_price), "trade price");
        r.trade_fee = 0;
        check(oms->engine->report(r), "trade applied");
    }
    oms::Position t0_position() const {
        oms::Position p;
        check(session->execution()->read_t0_position(88, code, exchange, &p), "T0 position available"); return p;
    }
};

void combined_and_cancel(const std::string& market) {
    Fixture f(market, 300);
    check(f.t0_position().total == 1300, "initial PE excluded from PI");
    const int id = f.round(930, 100);
    auto commands = f.commands();
    check(id > 0 && commands.size() == 1, "one combined physical order");
    const auto c = commands.front();
    check(c.intent.quantity == 400 && c.intent.external_quantity == 300 && c.intent.price == 100300,
        "400 shares at ask three, 300 external");
    check(c.intent.type == oms::OrderType::LimitThenCancel && c.intent.cancel_delay_ns == 1001000000LL,
        "combined order inherits T0 cancellation clock");
    f.fill(c, 200, 200, "first");
    f.fill(c, 200, 200, "first");
    auto p = f.t0_position();
    check(p.total == 1400 && p.bought == 100 && p.working_buy == 0, "T0 credited first, duplicate not counted");
    oms::Position physical;
    check(f.oms->engine->position(c.intent.instrument, &physical) && physical.total == 1200 &&
        physical.external_bought == 100 && physical.external_working_buy == 200, "physical allocations");
    oms::OrderView t0;
    check(f.session->execution()->read_order(88, id, &t0) && t0.terminal && t0.filled == 100 &&
        t0.command.intent.quantity == 100 && t0.known_amount == 10010000, "logical T0 completion");
    f.round(930);
    check(f.commands().size() == 1, "no execution retry within same minute");
    f.round(931);
    check(f.commands().size() == 1, "wait for cancel confirmation");
    f.fill(c, 100, 300, "during-cancel");
    check(f.oms->engine->report(f.report(c, oms::OrderState::Canceled, 300)), "cancel terminal");
    f.round(931);
    commands = f.commands();
    check(commands.size() == 2 && commands.back().intent.quantity == 100 &&
        commands.back().intent.external_quantity == 100, "replace only remainder after cancel-time fill");
    f.fill(commands.back(), 100, 100, "last");
    f.round(932);
    check(f.commands().size() == 2 && f.t0_position().total == 1400, "PE zero stops execution");
    oms::Quantity qty; oms::Money amount;
    check(f.session->execution()->read_day_fills(88, f.code, f.exchange, &qty, &amount) &&
        qty == 100 && amount == 10010000, "T0 day turnover excludes external fills");
}

void sell_and_conflict(const std::string& market) {
    Fixture f(market, -300);
    const int id = f.round(930, -100);
    const auto c = f.commands().front();
    check(id > 0 && c.intent.quantity == 400 && c.intent.price == 99800, "combined sell at bid three");
    f.fill(c, 200, 200, "sell", 9.99);
    check(f.t0_position().total == 600 && f.t0_position().sold == 100, "sell fills T0 first");
    Fixture opposite(market, -100);
    check(opposite.round(930, 100) > 0, "opposite T0 allowed");
    check(opposite.commands().size() == 1 && opposite.commands().front().intent.quantity == 100 &&
        opposite.commands().front().intent.external_quantity == 0, "opposite execution suppressed");
    opposite.round(930);
    check(opposite.commands().size() == 1, "conflict consumes execution minute");
}

void standalone_and_gate(const std::string& market) {
    Fixture f(market, 300);
    f.round(929); f.round(1130); f.round(1200); f.round(1457);
    check(f.commands().empty(), "no external orders outside continuous trading");
    f.healthy = false; f.round(930);
    check(f.commands().empty(), "health gate blocks external submission");
    f.healthy = true; f.round(931);
    check(f.commands().size() == 1 && f.commands().front().intent.quantity == 300 &&
        f.commands().front().intent.type == oms::OrderType::Limit, "standalone full execution quantity");
    check(!f.session->execution()->has_working_order(f.code), "external working order does not gate T0");
    // Same-day reconstruction uses persisted ownership, not startup PE again.
    f.session.reset(new strategy_runtime::StrategySession(f.config, market, 88, f.oms->execution, []() { return true; }));
    f.session->set_ready(true, true, true);
    f.round(932);
    check(f.commands().size() == 1, "recovered active execution cannot duplicate");
}

void prices_and_money(const std::string& market) {
    Fixture f(market, 300);
    const int id = f.round(930, 100);
    const auto c = f.commands().front();
    // Out-of-order priced reports retain execution-prefix attribution.
    f.fill(c, 100, 200, "second", 10.02);
    f.fill(c, 100, 100, "first", 10.01);
    oms::OrderView v;
    check(f.session->execution()->read_order(88, id, &v) && v.priced_quantity == 100 &&
        v.known_amount == 10010000, "T0 fill prices follow first 100 shares");
    auto decoded = oms::records::decode(oms::records::intent(0, c));
    check(decoded.at("data").at("intent").at("external_quantity") == 300 &&
        decoded.at("data").at("intent").at("external_delta") == 300, "allocation journal roundtrip");
    auto legacy = c; legacy.intent.external_quantity = 0;
    check(!oms::records::decode(oms::records::intent(0, legacy)).at("data").at("intent").count("external_quantity"),
        "legacy intent encoding retained");
    Fixture invalid(market, 100);
    auto book = invalid.book(); book.ms_market_data.ms_market_data[AskPrice1Index + 2] = 0;
    invalid.session->on_decision(invalid.code, book, std::function<void()>());
    check(invalid.commands().empty(), "missing third level blocks execution");
}

void timers_and_oms_admission(const std::string& market) {
    Fixture f(market, 300);
    auto book = f.book(929); book.MarketTime = 92959900.0;
    f.session->on_decision(f.code, book, std::function<void()>());
    f.session->on_timer(34200000000ULL);
    check(f.commands().size() == 1, "minute timer runs without prediction");
    f.session->on_timer(34200001000ULL);
    check(f.commands().size() == 1, "timer does not duplicate execution");
    const auto c = f.commands().front();
    // OMS, not the decision adapter, enforces the physical execution reservation.
    check(f.oms->execution->submit_allocated(88, f.code, f.exchange, 10.03, 100,
        LF_CHAR_Buy, LF_CHAR_Open, oms::OrderType::Limit, 0, {}, "overshoot", 100, 300) < 0,
        "OMS rejects duplicate external reservation");
    f.session->on_timer(34260000000ULL);
    oms::OrderView pending;
    check(f.oms->engine->order(c.id, &pending) && pending.cancel_requested, "stale quote timer still cancels old order");

    Fixture constrained(market, 300);
    // The combined quantity exceeds the OMS per-order cap; T0 alone fits.
    constrained.config["ins_params"][constrained.code + (market == "SH" ? ".SH" : ".SZ")]["external_delta"] = 1000001;
    constrained.config["ins_params"][constrained.code + (market == "SH" ? ".SH" : ".SZ")]["static_position"] = 1001001;
    constrained.session.reset(new strategy_runtime::StrategySession(constrained.config, market, 88,
        constrained.oms->execution, []() { return true; }));
    constrained.session->set_ready(true, true, true);
    check(constrained.round(930, 100) > 0 && constrained.commands().size() == 1 &&
        constrained.commands().front().intent.quantity == 100, "rejected combined order falls back to T0");

    Fixture crossing(market, 100);
    check(crossing.oms->execution->submit_limit(88, crossing.code, crossing.exchange, 10.01, 100,
        LF_CHAR_Sell, LF_CHAR_Close) > 0, "existing T0 sell admitted");
    crossing.round(930);
    check(crossing.commands().size() == 1, "OMS rejects external self-cross");

    oms::Config settings;
    settings.scope.account = {"paper", "cold-start"}; settings.scope.gateway = "cold";
    settings.scope.day = 20260904; settings.scope.source = 88; settings.instance = "cold";
    settings.enabled = true;
    oms::InstrumentRules rules; rules.lower_price = 10000; rules.upper_price = 10000000;
    settings.instruments[oms::Instrument{f.exchange, f.code}] = rules;
    oms::Capabilities capabilities; capabilities.simulated = true; capabilities.complete_snapshot = true;
    auto backend = std::make_shared<oms::ScriptedBackend>(capabilities);
    auto engine = oms::Engine::create(settings, backend);
    check(engine->start_epoch(1), "cold OMS epoch");
    auto execution = std::make_shared<strategy_runtime::OmsStrategyExecution>(engine, "cold");
    strategy_runtime::StrategySession cold(f.config, market, 88, execution, []() { return true; });
    cold.on_decision(f.code, f.book(), std::function<void()>());
    check(engine->account().admissions == 0, "market before first account snapshot does not abort session");

    Fixture cutoff(market, 100);
    cutoff.round(1129); cutoff.round(1130);
    oms::OrderView lunch;
    check(cutoff.commands().size() == 1 && cutoff.oms->engine->order(cutoff.commands().front().id, &lunch) &&
        lunch.cancel_requested, "lunch boundary cancels existing execution without replacement");
}
}

int main() {
    try {
        for (const std::string& market : {std::string("SH"), std::string("SZ")}) {
            combined_and_cancel(market); sell_and_conflict(market);
            standalone_and_gate(market); prices_and_money(market);
            timers_and_oms_admission(market);
        }
        std::cout << "external execution tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
