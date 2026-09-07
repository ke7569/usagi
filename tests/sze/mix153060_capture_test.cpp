#include "sze/runtime/mix153060_capture.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream input(path.c_str());
    assert(input.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

void fill_sample(mix153060::Sample* sample) {
    assert(sample != 0);
    sample->instrument = "000001";
    sample->exchange_time_us = 1784511000000000LL;
    sample->local_time_us = 1784511000000123LL;
    sample->app_sequence = 12346;
    sample->cut_index = 7;
    sample->row_in_stock_day = 3;
    sample->window_start_exchange_time_us = 1784510999000000LL;
    sample->window_start_app_sequence = 12000;
    sample->window_start_cut_index = 6;
    sample->last_price = 11.25;
    sample->mid_price = 11.245;
    sample->turnover = 123456.75;
    sample->volume = 10900;
    sample->amount_trigger = true;
    sample->change_trigger = true;
    for (std::size_t i = 0; i < mix153060::kFeatureCount; ++i) {
        sample->factors[i] = static_cast<float>(i) + 0.25f;
    }
    for (std::size_t i = 0; i < 10; ++i) {
        sample->bid_price[i] = 11.24 - static_cast<double>(i) * 0.01;
        sample->ask_price[i] = 11.25 + static_cast<double>(i) * 0.01;
        sample->bid_volume[i] = 1000 + i;
        sample->ask_volume[i] = 2000 + i;
    }
}

void verify_runtime_timing() {
    mix153060::StaticInputs inputs;
    inputs.instrument = "000001";
    inputs.trading_date = 20260720;
    inputs.average_amount = 1028944752.9933333;
    inputs.turnover_threshold = inputs.average_amount / 8000.0;
    inputs.pre_close = 10.78;
    inputs.upper_limit = 11.86;
    inputs.lower_limit = 9.70;
    inputs.history_volatility_20d = 0.011990912959201799;
    mix153060::Runtime runtime(inputs);
    assert(runtime.configured());

    mix153060::OrderEvent order;
    order.app_sequence = 1;
    order.exchange_time_us = 1784511000000000LL;
    order.local_time_us = order.exchange_time_us + 100;
    order.price = 10.77;
    order.volume = 1000;
    order.buy = true;

    mix153060::SampleBuffer samples;
    mix153060::EventTiming timing;
    runtime.on_order(order, &samples, &timing);
    assert(runtime.available());
    assert(timing.book_mutation_ns > 0);
    assert(timing.total_runtime_ns >= timing.book_mutation_ns);

    order.app_sequence = 2;
    ++order.exchange_time_us;
    ++order.local_time_us;
    order.price = 10.79;
    order.buy = false;
    runtime.on_order(order, &samples, &timing);
    assert(runtime.available());
    assert(timing.book_mutation_ns > 0);
    assert(timing.total_runtime_ns >= timing.book_mutation_ns);

    mix153060::TradeEvent trade;
    trade.app_sequence = 3;
    trade.exchange_time_us = order.exchange_time_us + 1;
    trade.local_time_us = order.local_time_us + 1;
    trade.price = 10.78;
    trade.volume = 100;
    trade.buy_order_id = 1;
    trade.sell_order_id = 2;
    runtime.on_trade(trade, &samples, &timing);
    assert(runtime.available());
    assert(timing.book_mutation_ns > 0);
    assert(timing.total_runtime_ns >= timing.book_mutation_ns);
}

}  // namespace

int main() {
    verify_runtime_timing();
    char directory_template[] = "/tmp/mix153060_capture_test_XXXXXX";
    char* directory = ::mkdtemp(directory_template);
    assert(directory != 0);

    mix153060::CaptureConfig disabled_config;
    mix153060::Capture disabled(disabled_config);
    assert(disabled.ready());
    assert(!disabled.enabled_for("000001"));

    mix153060::CaptureConfig invalid_config;
    invalid_config.enabled = true;
    invalid_config.directory = directory;
    mix153060::Capture invalid(invalid_config);
    assert(!invalid.ready());
    assert(!invalid.error().empty());

    std::string events_path;
    std::string samples_path;
    std::string resolutions_path;
    {
        mix153060::CaptureConfig config;
        config.enabled = true;
        config.directory = directory;
        config.prefix = "000001_test";
        config.instruments.push_back("000001.SZ");
        config.flush_rows = 64;
        mix153060::Capture capture(config);
        assert(capture.ready());
        assert(capture.enabled_for("000001"));
        assert(capture.enabled_for("000001.SZ"));
        assert(!capture.enabled_for("000002.SZ"));

        mix153060::OrderEvent order;
        order.app_sequence = 12345;
        order.exchange_time_us = 1784511000000000LL;
        order.local_time_us = 1784511000000100LL;
        order.price = 11.24;
        order.volume = 1000;
        order.buy = true;

        mix153060::EventTiming timing;
        timing.book_mutation_ns = 101;
        timing.sample_work_ns = 202;
        timing.total_runtime_ns = 303;
        capture.record_order("000001.SZ", order, 89, 1784511000000100000LL,
                             timing, true, 1);
        order.app_sequence = 12347;
        capture.record_order("000001.SZ", order, 89, 1784511000000100000LL,
                             timing, true, 1, false);
        capture.record_order("000002.SZ", order, 89, 1784511000000100000LL,
                             timing, true, 1);
        order.app_sequence = 12348;
        order.price = 11.25;
        order.kind = mix153060::OrderKind::kMarket;
        capture.record_market_resolution("000001.SZ", order, true, 89,
                                         1784511000000100000LL, false);

        mix153060::Sample sample;
        fill_sample(&sample);
        capture.record_sample("000001", sample, 89, 1784511000000200000LL,
                              0.125f, 404);
        sample.app_sequence = 12347;
        capture.record_sample("000001", sample, 89, 1784511000000200000LL,
                              0.5f, 606, false);
        capture.record_sample("000002", sample, 89, 1784511000000200000LL,
                              0.25f, 505);
        assert(read_lines(capture.events_path()).size() == 2);
        assert(read_lines(capture.samples_path()).size() == 2);
        assert(read_lines(capture.market_resolutions_path()).size() == 1);
        capture.flush();
        events_path = capture.events_path();
        samples_path = capture.samples_path();
        resolutions_path = capture.market_resolutions_path();
    }

    const std::vector<std::string> event_lines = read_lines(events_path);
    assert(event_lines.size() == 3);
    assert(event_lines[0].find("book_mutation_ns") != std::string::npos);
    assert(event_lines[0].find("sample_work_ns") != std::string::npos);
    assert(event_lines[0].find("source_continuity_valid") != std::string::npos);
    assert(event_lines[1].find("000001,order,limit,89") == 0);
    assert(event_lines[1].find(",101,202,303,1,") != std::string::npos);
    assert(event_lines[1].find(",1\n") != std::string::npos);
    assert(event_lines[2].find(",0\n") != std::string::npos);

    const std::vector<std::string> sample_lines = read_lines(samples_path);
    assert(sample_lines.size() == 3);
    assert(sample_lines[0].find("raw_prediction,model_latency_ns") != std::string::npos);
    assert(sample_lines[0].find("source_continuity_valid") != std::string::npos);
    for (std::size_t i = 0; i < mix153060::kFeatureCount; ++i) {
        assert(sample_lines[0].find(mix153060::factor_names()[i]) != std::string::npos);
    }
    assert(sample_lines[1].find("000001,") == 0);
    assert(sample_lines[1].find(",0.125,404,") != std::string::npos);
    assert(sample_lines[1].size() >= 2U);
    assert(sample_lines[1].compare(sample_lines[1].size() - 2U, 2U, ",1") == 0);
    assert(sample_lines[2].find(",0.5,606,") != std::string::npos);
    assert(sample_lines[2].size() >= 2U);
    assert(sample_lines[2].compare(sample_lines[2].size() - 2U, 2U, ",0") == 0);
    assert(sample_lines[1].find("000002") == std::string::npos);

    const std::vector<std::string> resolution_lines = read_lines(resolutions_path);
    assert(resolution_lines.size() == 2);
    assert(resolution_lines[0].find("resolution_source") != std::string::npos);
    assert(resolution_lines[1].find("000001,12348,") == 0);
    assert(resolution_lines[1].find(",11.25,1000,1,final-linked-fill,89,") !=
           std::string::npos);
    assert(resolution_lines[1].size() >= 2U);
    assert(resolution_lines[1].compare(resolution_lines[1].size() - 2U, 2U, ",0") == 0);

    assert(::unlink(events_path.c_str()) == 0);
    assert(::unlink(samples_path.c_str()) == 0);
    assert(::unlink(resolutions_path.c_str()) == 0);
    assert(::rmdir(directory) == 0);
    return 0;
}
