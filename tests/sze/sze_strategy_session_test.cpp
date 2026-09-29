#include "sze/runtime/SzeStrategySession.h"
#include "tests/oms/TestExecution.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

nlohmann::json config() {
    nlohmann::json value;
    value["market"] = "SZ";
    value["global_params"]["offset"] = 1.0;
    value["global_params"]["global_bias_factor"] = 1.0;
    value["global_params"]["position_base_line"] = 100000.0;
    value["global_params"]["position_limit"] = 1000.0;
    value["sze_startup_warmup_signals"] = 0;
    value["ins_params"]["000001.SZ"]["static_position"] = 1000;
    value["ins_params"]["000001.SZ"]["last_position"] = 0;
    return value;
}

mix153060::Sample sample(std::int64_t exchange_time,
                         std::int64_t local_time,
                         std::int64_t sequence) {
    mix153060::Sample value;
    value.instrument = "000001.SZ";
    value.exchange_time_us = exchange_time;
    value.local_time_us = local_time;
    value.app_sequence = sequence;
    value.mid_price = 10.05;
    value.last_price = 10.05;
    value.volume = 1000.0;
    value.turnover = 10000.0;
    for (std::size_t i = 0; i < 10U; ++i) {
        value.bid_price[i] = 10.0 - static_cast<double>(i) * 0.01;
        value.ask_price[i] = 10.1 + static_cast<double>(i) * 0.01;
        value.bid_volume[i] = 1000.0;
        value.ask_volume[i] = 1000.0;
    }
    return value;
}

sze_stream::ProcessedSample output(const mix153060::Sample& value,
                                   float prediction = 1.0f) {
    sze_stream::ProcessedSample result;
    result.sample = value;
    result.prediction = prediction;
    result.prediction_valid = true;
    return result;
}

void test_view_mapping_and_no_sse_dedup() {
    oms_test::ManagedFixture managed(config(), "SZ", 88);
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    bool healthy = true;
    sze_strategy::Session session(config(), 88, backend,
                                  [&healthy]() { return healthy; });
    session.set_ready(true, true, true);

    std::int64_t exchange = 0;
    assert(mix153060::parse_exchange_time_us("09:30:00.123", 20260904,
                                            &exchange));
    const mix153060::Sample first = sample(exchange, exchange + 27, 7);
    session.on_output(output(first, 100.0f));
    assert(managed.engine->account().admissions == 0);
    session.on_output(output(first, 100.0f));
    assert(managed.engine->account().admissions > 0);
    oms::OrderView order;
    assert(managed.engine->order(1, &order));
    assert(order.command.intent.instrument.code == "000001" &&
           order.command.intent.instrument.market == "SZE");
    assert(order.command.scope.source == 88 && order.command.intent.quantity > 0);
    oms::Report accepted;
    accepted.scope = managed.engine->scope();
    accepted.id = 1;
    accepted.broker_id = "B1";
    accepted.instrument = order.command.intent.instrument;
    accepted.side = order.command.intent.side;
    accepted.kind = oms::ReportKind::Order;
    accepted.state = oms::OrderState::Accepted;
    accepted.cumulative = 0;
    accepted.leaves = order.command.intent.quantity;
    managed.backend->publish(accepted);
    assert(session.signals() == 2U);
    const MSMarketDataField* view = session.last_view("000001");
    assert(view != 0);
    assert(view->InstrumentID == 1.0);
    assert(view->MarketTime == 93000123.0);
    assert(view->BidPrice1 == 10.0 && view->AskPrice1 == 10.1);
    assert(view->BidVolumeA == 1000.0 && view->AskVolumeA == 1000.0);
    assert(view->AppSeq == 7.0);
}

void test_invalid_prediction_or_book_is_filtered() {
    oms_test::ManagedFixture managed(config(), "SZ", 88, "sze-invalid");
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    sze_strategy::Session session(config(), 88, backend, []() { return true; });
    std::int64_t exchange = 0;
    assert(mix153060::parse_exchange_time_us("09:30:00.000", 20260904,
                                            &exchange));
    mix153060::Sample value = sample(exchange, exchange, 1);
    sze_stream::ProcessedSample invalid_prediction = output(value);
    invalid_prediction.prediction_valid = false;
    session.on_output(invalid_prediction);
    assert(session.signals() == 0U);
    value.bid_volume[0] = 0.0;
    session.on_output(output(value));
    assert(session.signals() == 0U);
}

void test_startup_position_sync_is_forwarded() {
    oms_test::ManagedFixture managed(config(), "SZ", 88, "sze-position");
    std::shared_ptr<StrategyExecution> backend = managed.execution;
    sze_strategy::Session session(config(), 88, backend, []() { return true; });
    std::map<std::string, std::pair<int, int> > positions;
    positions["000001"] = std::make_pair(1000, 1000);
    session.sync_startup_positions(positions);
}

void test_execution_runs_without_prediction() {
    auto runtime = config();
    runtime["ins_params"]["000001.SZ"]["static_position"] = 1300;
    runtime["ins_params"]["000001.SZ"]["external_delta"] = 300;
    oms_test::ManagedFixture managed(config(), "SZ", 88, "sze-external");
    sze_strategy::Session session(runtime, 88, managed.execution, []() { return true; });
    session.set_ready(true, true, true);
    std::int64_t exchange = 0;
    assert(mix153060::parse_exchange_time_us("09:30:00.000", 20260904, &exchange));
    auto value = output(sample(exchange, exchange, 1)); value.prediction_valid = false;
    session.on_output(value);
    oms::OrderView order;
    assert(managed.engine->order(1, &order) && order.command.intent.quantity == 300 &&
        order.command.intent.external_quantity == 300 && order.command.intent.price == 101200);
    assert(session.signals() == 0);
}

}  // namespace

int main() {
    test_view_mapping_and_no_sse_dedup();
    test_invalid_prediction_or_book_is_filtered();
    test_startup_position_sync_is_forwarded();
    test_execution_runs_without_prediction();
    return 0;
}
