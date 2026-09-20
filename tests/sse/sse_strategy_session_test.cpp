#include "common/oms/OrderLatency.h"
#include "sse/runtime/sse_strategy_session.h"
#include "common/contracts/legacy/LFConstants.h"
#include "tests/oms/TestExecution.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

nlohmann::json session_config() {
    nlohmann::json config;
    config["market"] = "SH";
    config["global_params"]["offset"] = 1.0;
    config["global_params"]["global_bias_factor"] = 1.0;
    config["global_params"]["position_base_line"] = 100000.0;
    config["global_params"]["position_limit"] = 1000.0;
    config["sze_startup_warmup_signals"] = 0;
    config["ins_params"]["600000.SH"]["static_position"] = 1000;
    config["ins_params"]["600000.SH"]["last_position"] = 0;
    config["ins_params"]["600001.SH"]["static_position"] = 1000;
    config["ins_params"]["600001.SH"]["last_position"] = 0;
    return config;
}

sse_tick::Level level(std::int64_t price_raw, std::uint64_t quantity) {
    sse_tick::Level value = {};
    value.price_raw = price_raw;
    value.quantity = quantity;
    value.order_count = 1;
    return value;
}

sse_stream::Output tick_output(const std::string& symbol,
                               std::uint64_t exchange_us,
                               float selected_prediction,
                               std::int64_t bid_price_raw,
                               std::int64_t ask_price_raw,
                               std::uint64_t tick_index,
                               bool hardware_timestamped = false) {
    sse_stream::Output output;
    output.kind = sse_stream::kTickOutput;
    output.tick.event.security_id = symbol;
    output.tick.event.time_of_day_micros = exchange_us;
    output.tick.event.tick_index = tick_index;
    output.tick.event.channel_no = 7;
    output.tick.prediction_valid = true;
    output.tick.prediction.selected = true;
    output.tick.prediction.selected_source = sse_hybrid_model::kTickSource;
    output.tick.prediction.selected_pred = selected_prediction;
    output.tick.bid_levels.assign(10U, level(bid_price_raw, 100000U));
    output.tick.ask_levels.assign(10U, level(ask_price_raw, 100000U));
    output.tick.last_trade_price =
        (static_cast<double>(bid_price_raw) + ask_price_raw) / 2000.0;
    output.tick.total_trade_volume = 100000U;
    output.tick.total_trade_turnover = 1000000.0;
    if (hardware_timestamped) {
        output.tick.provenance.timestamp_flags = static_cast<std::uint16_t>(
            deepwin_market_data::kKernelRealtimeTimestamp |
            deepwin_market_data::kHardwareReceiveTimestamp |
            deepwin_market_data::kHardwareTimestampRequested);
    }
    return output;
}

sse_stream::Output snapshot_output(const std::string& symbol,
                                   std::uint64_t exchange_us,
                                   float selected_prediction,
                                   double bid_price,
                                   double ask_price,
                                   std::uint64_t sequence) {
    sse_stream::Output output;
    output.kind = sse_stream::kSnapshotOutput;
    output.snapshot.snapshot.security_id = symbol;
    output.snapshot.snapshot.time_of_day_micros = exchange_us;
    output.snapshot.snapshot.sequence = sequence;
    output.snapshot.snapshot.last_price = (bid_price + ask_price) / 2.0;
    output.snapshot.snapshot.volume = 1000;
    output.snapshot.snapshot.turnover = 100000.0;
    for (std::size_t i = 0; i < 5U; ++i) {
        output.snapshot.snapshot.bid_prices[i] = bid_price - i * 0.01;
        output.snapshot.snapshot.ask_prices[i] = ask_price + i * 0.01;
        output.snapshot.snapshot.bid_volumes[i] = 1000;
        output.snapshot.snapshot.ask_volumes[i] = 1000;
    }
    output.snapshot.prediction_valid = true;
    output.snapshot.prediction.selected = true;
    output.snapshot.prediction.selected_source = sse_hybrid_model::kSnapshotSource;
    output.snapshot.prediction.selected_pred = selected_prediction;
    return output;
}

LFRtnOrderField order_report(const char* symbol) {
    LFRtnOrderField value = {};
    std::strncpy(value.InstrumentID, symbol, sizeof(value.InstrumentID) - 1U);
    return value;
}

LFRtnTradeField trade_report(const char* symbol) {
    LFRtnTradeField value = {};
    std::strncpy(value.InstrumentID, symbol, sizeof(value.InstrumentID) - 1U);
    return value;
}

void test_session_processing_and_protection() {
    bool healthy = true;
    oms_test::ManagedFixture managed(session_config(), "SH", 28);
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    sse_strategy::Session session(
        session_config(), 28, backend, [&healthy]() { return healthy; });
    assert(!session.execution()->permits_new_orders());

    const sse_stream::Output a_snapshot = snapshot_output(
        "600000", 34200001000ULL, 100.0, 10.0, 10.1, 1U);
    const sse_stream::Output b_snapshot = snapshot_output(
        "600001", 34200001000ULL, -100.0, 20.0, 20.1, 2U);
    managed.engine->advance_to(1000000000LL);
    session.on_output(a_snapshot);
    session.on_output(b_snapshot);
    assert(managed.engine->account().admissions == 0);

    session.set_ready(true, true, true);
    const sse_stream::Output a_tick = tick_output(
        "600000", 34500000000ULL, 100.0, 10000, 10100, 3U);
    const sse_stream::Output b_tick = tick_output(
        "600001", 34500000000ULL, -100.0, 20000, 20100, 4U);
    managed.engine->advance_to(2000000000LL);
    session.on_output(a_tick);
    session.on_output(b_tick);
    assert(session.signals() == 4U);
    assert(managed.engine->account().admissions >= 2U);
    oms::OrderView first_order;
    assert(managed.engine->order(1, &first_order));
    oms::Report accepted;
    accepted.scope = managed.engine->scope();
    accepted.id = 1;
    accepted.broker_id = "B1";
    accepted.instrument = first_order.command.intent.instrument;
    accepted.side = first_order.command.intent.side;
    accepted.kind = oms::ReportKind::Order;
    accepted.state = oms::OrderState::Accepted;
    accepted.cumulative = 0;
    accepted.leaves = first_order.command.intent.quantity;
    managed.backend->publish(accepted);
    oms::Report trade = accepted;
    trade.kind = oms::ReportKind::Trade;
    trade.trade_id = "T1";
    trade.trade_quantity = 100;
    trade.cumulative_after = 100;
    trade.trade_price = 100000;
    trade.trade_fee = 0;
    managed.backend->publish(trade);

    const std::size_t signal_count = session.signals();
    session.on_output(a_tick);
    assert(session.signals() == signal_count);  // Same symbol/time is deduplicated.

    const MSMarketDataField* a_view = session.last_view("600000");
    const MSMarketDataField* b_view = session.last_view("600001");
    assert(a_view != 0 && b_view != 0 && a_view != b_view);
    assert(a_view->BidPrice1 == 10.0 && a_view->AskPrice1 == 10.1);
    assert(b_view->BidPrice1 == 20.0 && b_view->AskPrice1 == 20.1);
    assert(a_view->BidPriceA == 10.0 && a_view->AskPriceA == 10.1);

    healthy = false;
    const std::size_t before_unhealthy = managed.engine->account().admissions;
    managed.engine->advance_to(3000000000LL);
    session.on_output(tick_output("600000", 34500001000ULL, 100.0,
                                  10000, 10100, 5U));
    assert(managed.engine->account().admissions == before_unhealthy);
    healthy = true;

    const std::size_t orders_before_stop = managed.engine->account().admissions;
    session.begin_stop();
    managed.engine->advance_to(4000000000LL);
    session.on_output(tick_output("600000", 34500002000ULL, 100.0,
                                  10000, 10100, 6U));
    const std::size_t orders_after_stop = managed.engine->account().admissions;
    assert(orders_after_stop == orders_before_stop);
}

void test_paper_cancel_deadline() {
    bool healthy = true;
    oms_test::ManagedFixture managed(session_config(), "SH", 28, "sse-cancel");
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    int cancel_count = 0;
    managed.backend->cancel_hook = [&cancel_count](const oms::Command&) {
        ++cancel_count;
        oms::SendResult result; result.disposition = oms::SendDisposition::Submitted; return result;
    };
    sse_strategy::Session session(
        session_config(), 28, backend, [&healthy]() { return healthy; });
    session.set_ready(true, true, true);
    managed.engine->advance_to(1000000000LL);
    session.on_output(snapshot_output("600000", 34200001000ULL, 100.0,
                                      10.0, 10.1, 1U));
    managed.engine->advance_to(2000000000LL);
    session.on_output(tick_output("600000", 34500000000ULL, 100.0,
                                  10000, 10100, 2U));
    assert(cancel_count == 0U);
    managed.engine->advance_to(4000000000LL);
    assert(cancel_count > 0U);
}

void test_hardware_tick_waits_for_batch_end() {
    bool healthy = true;
    oms_test::ManagedFixture managed(session_config(), "SH", 28, "sse-batch-end");
    sse_strategy::Session session(
        session_config(), 28, managed.execution, [&healthy]() { return healthy; });
    session.set_ready(true, true, true);
    managed.engine->advance_to(1000000000LL);
    session.on_output(tick_output("600000", 34500000000ULL, 100.0,
                                  10000, 10100, 1U, true));
    assert(session.signals() == 0U);
    sse_stream::Output marker;
    marker.kind = sse_stream::kBatchEndOutput;
    marker.batch_end.batch_id = 1U;
    marker.batch_end.packet_count = 1U;
    marker.batch_end.candidate_count = 1U;
    managed.engine->advance_to(2000000000LL);
    session.on_output(marker);
    assert(session.signals() == 1U);
}

void test_source_selection_validation() {
    bool healthy = true;
    oms_test::ManagedFixture managed(session_config(), "SH", 28, "sse-source");
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    sse_strategy::Session session(
        session_config(), 28, backend, [&healthy]() { return healthy; });
    std::map<std::string, std::pair<int, int> > positions;
    positions["600000"] = std::make_pair(1000, 1000);
    positions["600001"] = std::make_pair(1000, 1000);
    session.core().sync_startup_positions(positions);
    sse_stream::Output invalid = snapshot_output(
        "600000", 34200001000ULL, 100.0, 10.0, 10.1, 1U);
    invalid.snapshot.prediction.selected_source = sse_hybrid_model::kTickSource;
    session.set_ready(true, true, true);
    bool threw = false;
    try {
        session.on_output(invalid);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    assert(!session.execution()->permits_new_orders());
}

void test_static_conflict_blocks_already_queued_signal() {
    oms_test::ManagedFixture managed(session_config(), "SH", 28, "sse-static-gate");
    sse_strategy::Session session(session_config(), 28, managed.execution, []() { return true; });
    bool first_stock_valid = true;
    session.set_instrument_gate([&](const std::string& code) {
        return code != "600000" || first_stock_valid;
    });
    session.set_ready(true, true, true);
    managed.engine->advance_to(1000000000LL);
    session.on_output(tick_output("600000", 34500000000ULL, 100.0,
                                  10000, 10100, 1U, true));
    session.on_output(tick_output("600001", 34500000000ULL, 100.0,
                                  20000, 20100, 2U, true));
    first_stock_valid = false;  // Snapshot conflict arrives before batch dispatch.
    sse_stream::Output marker;
    marker.kind = sse_stream::kBatchEndOutput;
    session.on_output(marker);
    assert(session.signals() == 1U);
    assert(session.last_view("600001") != 0);
    const MSMarketDataField* blocked = session.last_view("600000");
    assert(blocked == 0 || blocked->MarketTime == 0);
}

void test_live_latency_windows() {
    auto cfg=session_config();cfg["model_version"]="v0.6";
    oms_test::ManagedFixture managed(cfg, "SH", 28, "sse-latency-window");
    sse_strategy::Session session(cfg, 28, managed.execution, [](){return false;});
    session.enable_live_latency(true);
    auto output=tick_output("600000",34500000000ULL,100.0,10000,10100,1U,true);
    output.tick.prediction.multi_head=true;output.tick.prediction.heads={{0,0,0,0}};
    sse_stream::Output marker;marker.kind=sse_stream::kBatchEndOutput;
    output.tick.provenance.monotonic_ns=order_latency::now_ns()-1000000000ULL;
    session.on_output(output);
    assert(session.live_latency()["count"]==0); // Still buffered: strategy has not run.
    session.on_output(marker);
    auto first=session.live_latency();
    assert(first["count"]==1 && first["since_previous_status"]["count"]==1);
    assert(first["since_previous_status"]["max_us"].get<double>()>=1000000.0);
    output.tick.provenance.monotonic_ns=order_latency::now_ns()-100000ULL;
    session.on_output(output);session.on_output(marker);
    auto second=session.live_latency();
    assert(second["count"]==2 && second["since_previous_status"]["count"]==1);
    assert(second["since_previous_status"]["max_us"].get<double>()<first["max_us"].get<double>());
    auto empty=session.live_latency();
    assert(empty["count"]==2 && empty["since_previous_status"]["count"]==0);
    assert(empty["since_previous_status"]["max_us"]==0.0);
    session.enable_live_latency(false);session.on_output(output);session.on_output(marker);
    assert(session.live_latency()["count"]==0);
}

void test_completed_batch_preserves_orders_and_cancel_deadlines(bool independent=false) {
    auto cfg=session_config();cfg["model_version"]="v0.6";
    for(auto it=cfg["ins_params"].begin();it!=cfg["ins_params"].end();++it) it.value()["Open"]=10.0;
    oms_test::ManagedFixture reference(cfg,"SH",28,"batch-reference");
    oms_test::ManagedFixture candidate(cfg,"SH",28,"batch-candidate");
    std::vector<oms::Command> old_commands,new_commands;
    const auto capture=[](std::vector<oms::Command>& commands,const oms::Command& command) {
        commands.push_back(command);oms::SendResult result;
        result.disposition=oms::SendDisposition::Submitted;
        result.broker_id="B"+std::to_string(command.id);return result;
    };
    reference.backend->submit_hook=[&](const oms::Command& c){return capture(old_commands,c);};
    reference.backend->cancel_hook=[&](const oms::Command& c){return capture(old_commands,c);};
    candidate.backend->submit_hook=[&](const oms::Command& c){return capture(new_commands,c);};
    candidate.backend->cancel_hook=[&](const oms::Command& c){return capture(new_commands,c);};
    sse_strategy::Session old_session(cfg,28,reference.execution,[](){return true;});
    sse_strategy::Session new_session(cfg,28,candidate.execution,[](){return true;});
    old_session.set_ready(true,true,true);new_session.set_ready(true,true,true);
    reference.engine->advance_to(2000000000LL);candidate.engine->advance_to(2000000000LL);
    std::vector<sse_stream::Output> batch;
    for(const auto& code:{std::string("600000"),std::string("600001")}) {
        auto output=tick_output(code,34500000000ULL,100.0,10000,10100,1U,true);
        output.tick.prediction.multi_head=true;
        output.tick.prediction.heads={{100.0f,1.0f,2.0f,3.0f}};
        output.tick.provenance.batch_id=1;output.tick.provenance.batch_emitted_ns=2000000000ULL;
        batch.push_back(output);old_session.on_output(output);
        if(independent)new_session.on_closed_prediction(output);
    }
    assert(old_commands.empty());
    sse_stream::Output marker;marker.kind=sse_stream::kBatchEndOutput;
    batch.push_back(marker);
    if(independent)assert(new_commands.size()==2);
    old_session.on_output(marker);
    if(!independent)new_session.on_completed_batch(batch);
    assert(old_commands.size()==2 && new_commands.size()==2);
    reference.engine->advance_to(3000000000LL-1);candidate.engine->advance_to(3000000000LL-1);
    assert(old_commands.size()==2 && new_commands.size()==2);
    reference.engine->advance_to(3000000000LL);candidate.engine->advance_to(3000000000LL);
    assert(old_commands.size()==4 && new_commands.size()==4);
    for(std::size_t i=0;i<old_commands.size();++i) {
        const auto& a=old_commands[i];const auto& b=new_commands[i];
        assert(a.id==b.id && a.cancel==b.cancel && a.time_ns==b.time_ns);
        assert(a.intent.instrument==b.intent.instrument && a.intent.side==b.intent.side);
        assert(a.intent.price==b.intent.price && a.intent.quantity==b.intent.quantity);
        assert(a.intent.signal_id==b.intent.signal_id && a.intent.type==b.intent.type);
        assert(a.intent.cancel_delay_ns==b.intent.cancel_delay_ns);
    }
    assert(reference.engine->account().cash_reserved==candidate.engine->account().cash_reserved);
}

void test_completed_batch_static_gate_and_snapshot_reference() {
    auto cfg=session_config();cfg["model_version"]="v0.6";
    oms_test::ManagedFixture managed(cfg,"SH",28,"batch-reference-gate");
    sse_strategy::Session session(cfg,28,managed.execution,[](){return true;});
    session.set_ready(true,true,true);
    session.set_instrument_gate([](const std::string& code){return code=="600001";});
    managed.engine->advance_to(2000000000LL);
    std::vector<sse_stream::Output> batch;
    for(const auto& code:{std::string("600000"),std::string("600001")}) {
        auto tick=tick_output(code,34500000000ULL,100.0,10000,10100,1U,true);
        tick.tick.prediction.multi_head=true;tick.tick.prediction.heads={{100,1,2,3}};
        batch.push_back(tick);
    }
    // References later in the committed frame must be visible before decisions.
    for(const auto& code:{std::string("600000"),std::string("600001")}) {
        auto snapshot=snapshot_output(code,34500000000ULL,0.0,10.0,10.1,1U);
        snapshot.snapshot.snapshot.open_price=10.0;snapshot.snapshot.prediction_valid=false;
        batch.push_back(snapshot);
    }
    sse_stream::Output marker;marker.kind=sse_stream::kBatchEndOutput;batch.push_back(marker);
    session.on_completed_batch(batch);
    assert(session.signals()==1 && managed.engine->account().admissions==1);
}

}  // namespace

int main() {
    test_live_latency_windows();
    test_session_processing_and_protection();
    test_paper_cancel_deadline();
    test_hardware_tick_waits_for_batch_end();
    test_source_selection_validation();
    test_static_conflict_blocks_already_queued_signal();
    test_completed_batch_preserves_orders_and_cancel_deadlines();
    test_completed_batch_preserves_orders_and_cancel_deadlines(true);
    test_completed_batch_static_gate_and_snapshot_reference();
    std::cout << "sse_strategy_session_test: PASS" << std::endl;
    return 0;
}
