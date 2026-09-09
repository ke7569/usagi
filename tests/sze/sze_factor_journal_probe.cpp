// Offline measurement only. Never attaches SHM or opens an execution backend.
#include "common/config/StreamConfigJson.h"
#include "sze/market_data/SZEProtocol.h"
#include "sze/market_data/SZERecoverable.h"
#include "sze/runtime/mix153060_live_adapter.h"
#include "sze/sampling/mix153060_runtime.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <vector>

using nlohmann::json;
static json stats(std::vector<uint64_t>& values) {
    if (values.empty()) return json();
    std::sort(values.begin(), values.end());
    long double sum = 0;
    for (auto v : values) sum += v;
    return json{{"n", values.size()}, {"p50_ns", values[(values.size()-1)/2]},
        {"p95_ns", values[(values.size()-1)*95/100]},
        {"p99_ns", values[(values.size()-1)*99/100]},
        {"max_ns", values.back()}, {"mean_ns", double(sum/values.size())}};
}
int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::runtime_error("usage: probe JOURNAL PROFILE MAX_EVENTS DUMP");
        const json profile = load_stream_json(argv[2]);
        const auto& recovery = profile.at("recovery");
        sze_recovery::JournalConfig config;
        config.directory = argv[1];
        config.prefix = recovery.at("journal_prefix").get<std::string>();
        config.trading_day = profile.at("trading_day").get<uint32_t>();
        config.source_id = recovery.at("source_id").get<uint32_t>();
        config.segment_bytes = recovery.at("journal_segment_mb").get<uint64_t>() * 1024 * 1024;
        config.max_payload_bytes = recovery.at("journal_max_payload_bytes").get<uint32_t>();
        config.generation = recovery.at("expected_generation").get<uint64_t>();
        const uint64_t limit = std::stoull(argv[3]);
        std::map<std::string, std::unique_ptr<mix153060::Runtime> > runtimes;
        for (const auto& item : profile.at("instruments")) {
            mix153060::StaticInputs s;
            s.instrument = item.at("instrument").get<std::string>();
            s.trading_date = config.trading_day;
            s.average_amount = item.at("average_amount").get<double>();
            s.turnover_threshold = item.at("turnover_threshold").get<double>();
            s.free_share = item.at("free_share").get<double>();
            s.pre_close = item.at("pre_close").get<double>();
            s.upper_limit = item.at("upper_limit").get<double>();
            s.lower_limit = item.at("lower_limit").get<double>();
            s.history_volatility_20d = item.at("history_volatility_20d").get<double>();
            runtimes[s.instrument].reset(new mix153060::Runtime(s));
        }
        sze_recovery::JournalReader reader;
        const auto opened = reader.open(config);
        if (opened.status != sze_recovery::kJournalOk || opened.corrupt_tail ||
            opened.continuity_state == sze_recovery::kContinuityInvalid)
            throw std::runtime_error("invalid journal");
        std::ofstream dump(argv[4]);
        if (!dump) throw std::runtime_error("cannot open dump");
        dump << std::setprecision(17);
        std::vector<unsigned char> payload(config.max_payload_bytes);
        std::vector<uint64_t> sampling, mutations, total;
        uint64_t records = 0, selected = 0, samples = 0;
        const auto start = std::chrono::steady_clock::now();
        for (; records < limit; ++records) {
            sze_recovery::CanonicalEvent event = {};
            const auto status = reader.next(&event, payload.data(), payload.size());
            if (status == sze_recovery::kJournalEnd || status == sze_recovery::kJournalWouldBlock) break;
            if (status != sze_recovery::kJournalOk || event.event_id != records+1)
                throw std::runtime_error("journal read or sequence failure");
            LFL2OrderField order = {};
            LFL2TradeField trade = {};
            sze_md::DecodeFailureReason failure = sze_md::DecodeFailureReason::kNone;
            const auto kind = sze_md::decode_recovery_record(event, payload.data(), event.payload_size,
                                                             &order, &trade, &failure);
            const bool is_order = kind == sze_md::DecodeStatus::kOrder;
            if (!is_order && kind != sze_md::DecodeStatus::kExecution)
                throw std::runtime_error("decode failed");
            const std::string code = is_order ? order.InstrumentID : trade.InstrumentID;
            const auto found = runtimes.find(code);
            if (found == runtimes.end()) continue;
            auto& runtime = *found->second;
            std::string error;
            mix153060::SampleBuffer buffer;
            mix153060::EventTiming timing;
            if (is_order) {
                mix153060::OrderEvent e;
                if (!mix153060::normalize_order_event(order, config.trading_day, 0, &e, &error))
                    throw std::runtime_error(error);
                runtime.on_order(e, &buffer, &timing);
            } else {
                mix153060::TradeEvent e;
                if (!mix153060::normalize_trade_event(trade, config.trading_day, 0, &e, &error))
                    throw std::runtime_error(error);
                runtime.on_trade(e, &buffer, &timing);
            }
            if (!runtime.available()) throw std::runtime_error(code + " event=" +
                std::to_string(event.event_id) + " " + runtime.failure_reason());
            ++selected;
            mutations.push_back(timing.book_mutation_ns);
            total.push_back(timing.total_runtime_ns);
            if (buffer.count) sampling.push_back(timing.sample_work_ns);
            for (size_t i = 0; i < buffer.count; ++i) {
                const auto& s = buffer.values[i];
                dump << code << ',' << event.event_id << ',' << s.app_sequence << ','
                     << s.exchange_time_us << ',' << s.cut_index << ',' << s.row_in_stock_day << ','
                     << s.window_start_app_sequence << ',' << s.window_start_exchange_time_us;
                for (float f : s.factors) dump << ',' << f;
                dump << '\n';
                ++samples;
            }
        }
        dump.flush();
        if (!dump) throw std::runtime_error("dump write failed");
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        std::cout << json{{"records",records},{"selected",selected},{"samples",samples},
            {"seconds_with_io",seconds},{"sample_work",stats(sampling)},
            {"book_mutation",stats(mutations)},{"total_runtime",stats(total)}}.dump() << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
