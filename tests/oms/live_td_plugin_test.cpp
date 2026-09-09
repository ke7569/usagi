#include "common/execution/LiveTd.h"
#include "common/execution/OmsStrategyExecution.h"
#include "sse/runtime/sse_strategy_session.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <set>
#include <unistd.h>

using namespace oms;
using namespace strategy_runtime;
namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
Scope scope() {
    Scope s; s.account.broker = "fake"; s.account.account = "test-account";
    s.gateway = "fake"; s.day = 20260909; s.epoch = 17; s.source = 190; return s;
}
Instrument instrument() { Instrument i; i.market = "SSE"; i.code = "600000"; return i; }
Config config(const Scope& s) {
    Config c; c.scope = s; c.scope.epoch = 0; c.instance = "live-td-test";
    c.enabled = true; c.ownership = OwnershipMode::Simulation;
    InstrumentRules rules; rules.lower_price = 90000; rules.upper_price = 110000;
    c.instruments[instrument()] = rules; return c;
}
Command command(const Scope& s) {
    Command c; c.scope = s; c.id = 7; c.intent.owner = "strategy";
    c.intent.intent_id = "i7"; c.intent.instrument = instrument();
    c.intent.side = Side::Buy; c.intent.price = 100000; c.intent.quantity = 100; return c;
}
std::string read(const std::string& path) {
    std::ifstream input(path.c_str());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
void check_lifecycle(const std::string& path, const std::string& mode) {
    const std::string text = read(path);
    const auto create = text.find(mode), stop = text.find("stop\n"),
               destroy = text.find("destroy\n"), unload = text.find("unload\n");
    require(create != std::string::npos && stop != std::string::npos &&
            destroy != std::string::npos && unload != std::string::npos &&
            create < stop && stop < destroy && destroy < unload,
            "session must stop and die before plugin unload: " + text);
}
void test_loader_rejects_invalid_input(const std::string& library, const std::string& dir,
                                     const std::string& missing_symbols) {
    std::set<Instrument> universe; universe.insert(instrument());
    for (int variant = 0; variant < 4; ++variant) {
        bool rejected = false;
        try {
            LiveTdPlugin plugin(variant == 0 ? "relative-library" :
                (variant == 1 ? dir + "/absent.so" : (variant == 2 ? missing_symbols : library)),
                variant == 3 ? dir + "/reject-after-gates" : dir + "/invalid",
                scope(), universe, false);
        } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "loader accepted relative, missing, invalid-symbol or failed-create library");
    }
}
void test_query_only_gate(const std::string& library, const std::string& trace) {
    const Scope s = scope(); std::set<Instrument> universe; universe.insert(instrument());
    {
        LiveTdPlugin plugin(library, trace, s, universe, false);
        auto backend = plugin.session().backend();
        bool snapshot_seen = false;
        backend->bind(Backend::ReportSink(), [&](const Snapshot& snap) {
            snapshot_seen = snap.token == 1 && snap.positions_success;
        });
        std::string error; require(plugin.session().connect(1000000, &error), "query-only connect");
        backend->advance_to(1);
        Command request = command(s);
        require(backend->submit(request).error.failed(), "allow_orders=false permitted submit");
        request.cancel = true;
        require(backend->cancel(request).error.failed(), "allow_orders=false permitted cancel");
        require(!backend->query(s, 1).failed(), "query-only must permit account query");
        backend->advance_to(2); require(snapshot_seen, "query-only snapshot missing");
        require(plugin.session().status().find("submits=0 cancels=0 queries=1") != std::string::npos,
                "query-only sent a trading command");
        backend->unbind(); plugin.session().stop(); backend.reset();
    }
    check_lifecycle(trace, "create-query-only\n");
}
nlohmann::json strategy_config() {
    nlohmann::json legacy;
    legacy["market"] = "SH";
    legacy["global_params"] = {{"offset", 1.0}, {"position_base_line", 100000.0},
        {"position_limit", 1.0}, {"global_bias_factor", 1.0}};
    legacy["sze_startup_warmup_signals"] = 0;
    legacy["ins_params"]["600000.SH"] = {{"static_position", 200}, {"last_position", -200},
        {"min_order_size", 100}, {"vol_unit", 100}};
    return legacy;
}
sse_stream::Output positive_prediction(std::uint64_t index) {
    sse_stream::Output output; output.kind = sse_stream::kTickOutput;
    output.tick.event.security_id = "600000";
    output.tick.event.time_of_day_micros = 34500000000ULL + index * 1000;
    output.tick.event.tick_index = index; output.tick.event.channel_no = 7;
    output.tick.prediction_valid = true; output.tick.prediction.selected = true;
    output.tick.prediction.selected_source = sse_hybrid_model::kTickSource;
    output.tick.prediction.selected_pred = 100.0f;
    sse_tick::Level bid = {}, ask = {};
    bid.price_raw = 9990; ask.price_raw = 10010; bid.quantity = ask.quantity = 100000;
    bid.order_count = ask.order_count = 1;
    output.tick.bid_levels.assign(10, bid); output.tick.ask_levels.assign(10, ask);
    output.tick.last_trade_price = 10.0; output.tick.total_trade_volume = 1000;
    output.tick.total_trade_turnover = 10000.0; return output;
}
void test_real_strategy_to_plugin_report(const std::string& library, const std::string& trace) {
    const Scope s = scope(); std::set<Instrument> universe; universe.insert(instrument());
    {
        LiveTdPlugin plugin(library, trace, s, universe, true);
        auto engine = Engine::create(config(s), plugin.session().backend());
        plugin.session().attach(engine); require(engine->start_epoch(s.epoch, false), "start OMS epoch");
        std::string error; require(plugin.session().connect(1000000, &error), "connect fake TD");
        engine->advance_to(1000);
        require(engine->begin_reconcile(1), "request real backend query path");
        require(!engine->account().ready, "query must wait for owner callback delivery");
        engine->advance_to(2000);
        require(engine->account().ready, "backend query did not certify account: " + engine->account().reason);
        Position position; require(engine->position(instrument(), &position) && position.total == 0,
            "actual queried zero holding must enter strategy");
        std::shared_ptr<OmsStrategyExecution> execution(new OmsStrategyExecution(engine, "strategy"));
        bool healthy = true;
        {
            sse_strategy::Session session(strategy_config(), 190, execution, [&]() { return healthy; });
            session.set_ready(true, true, true);
            session.on_output(positive_prediction(1));
            session.on_output(positive_prediction(2));
            engine->drain();
            if (engine->account().admissions != 1) {
                std::cerr << "signals=" << session.signals() << " reason=" << engine->account().reason
                    << " rejects=" << engine->account().rejections << " " << plugin.session().status() << "\n";
                for (const auto& event : engine->audit_events())
                    std::cerr << event.type << " " << event.detail << "\n";
            }
            require(engine->account().admissions == 1, "positive prediction failed to submit through ZStrategy");
            require(plugin.session().status().find("submits=1") != std::string::npos,
                "strategy order did not reach plugin sender");
            engine->advance_to(3000);
            OrderView view; require(engine->order(1, &view) && view.state == OrderState::Accepted &&
                view.command.intent.side == Side::Buy && view.command.intent.quantity >= 100,
                "deferred plugin report failed to update strategy order");
            session.on_output(positive_prediction(3)); engine->drain();
            require(engine->account().admissions == 1, "single-flight allowed duplicate strategy order");
            require(!engine->cancel("strategy", 1).failed(), "cancel through plugin failed");
            engine->drain(); engine->advance_to(4000);
            require(engine->order(1, &view) && view.state == OrderState::Canceled && view.terminal,
                "plugin cancel report failed to close order");
            healthy = false; session.on_output(positive_prediction(4));
            require(engine->account().admissions == 1, "unhealthy transport allowed new strategy order");
            session.begin_stop();
        }
        execution.reset(); engine->begin_stop(); plugin.session().stop(); engine.reset();
    }
    check_lifecycle(trace, "create-orders\n");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: live_td_plugin_test FAKE_TD_LIBRARY MISSING_SYMBOL_LIBRARY");
        char temporary[] = "/tmp/live-td-plugin-test-XXXXXX";
        const char* created = ::mkdtemp(temporary); require(created != 0, "mkdtemp");
        const std::string dir(created);
        test_loader_rejects_invalid_input(argv[1], dir, argv[2]);
        test_query_only_gate(argv[1], dir + "/query.trace");
        test_real_strategy_to_plugin_report(argv[1], dir + "/live.trace");
        ::unlink((dir + "/reject-after-gates").c_str());
        ::unlink((dir + "/query.trace").c_str()); ::unlink((dir + "/live.trace").c_str());
        require(::rmdir(dir.c_str()) == 0, "remove test directory");
    } catch (const std::exception& error) {
        std::cerr << "live_td_plugin_test: " << error.what() << std::endl; return EXIT_FAILURE;
    }
    std::cout << "live_td_plugin_test ok\n"; return EXIT_SUCCESS;
}
