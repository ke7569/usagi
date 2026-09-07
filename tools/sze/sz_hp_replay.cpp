#include "third_party/nlohmann/json.hpp"
#include "sze/factors/sz_hp_factor_adapter.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

typedef std::map<std::string, std::string> CsvRow;

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    size_t begin = 0;
    while (begin <= line.size()) {
        const size_t end = line.find(',', begin);
        if (end == std::string::npos) {
            fields.push_back(line.substr(begin));
            break;
        }
        fields.push_back(line.substr(begin, end - begin));
        begin = end + 1;
    }
    return fields;
}

std::string trim(const std::string& value) {
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string field(const CsvRow& row, const char* name) {
    const CsvRow::const_iterator it = row.find(name);
    return it == row.end() ? std::string() : it->second;
}

double as_double(const std::string& value, double fallback = 0.0) {
    if (value.empty()) {
        return fallback;
    }
    char* end = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    return end == value.c_str() || *end != '\0' ? fallback : parsed;
}

int64_t as_int64(const std::string& value, int64_t fallback = 0) {
    if (value.empty()) {
        return fallback;
    }
    char* end = 0;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    return end == value.c_str() || *end != '\0'
               ? fallback
               : static_cast<int64_t>(parsed);
}

void copy_text(char* destination, size_t size, const std::string& value) {
    if (destination == 0 || size == 0) {
        return;
    }
    std::memset(destination, 0, size);
    std::strncpy(destination, value.c_str(), size - 1);
}

LFL2OrderField make_order(const CsvRow& row) {
    LFL2OrderField event;
    std::memset(&event, 0, sizeof(event));
    copy_text(event.InstrumentID, sizeof(event.InstrumentID), field(row, "instrument"));
    copy_text(event.ExchangeID, sizeof(event.ExchangeID), "SZ");
    copy_text(event.OrderTime, sizeof(event.OrderTime), field(row, "time"));
    copy_text(event.OrderKind, sizeof(event.OrderKind), field(row, "side"));
    copy_text(event.OrdType, sizeof(event.OrdType), field(row, "order_type"));
    event.Price = as_double(field(row, "price"));
    event.Volume = as_double(field(row, "quantity"));
    event.ApplSeqNum = as_int64(field(row, "sequence"));
    event.BizIndex = event.ApplSeqNum;
    event.IsLast = 1;
    return event;
}

LFL2TradeField make_trade(const CsvRow& row) {
    LFL2TradeField event;
    std::memset(&event, 0, sizeof(event));
    copy_text(event.InstrumentID, sizeof(event.InstrumentID), field(row, "instrument"));
    copy_text(event.ExchangeID, sizeof(event.ExchangeID), "SZ");
    copy_text(event.TradeTime, sizeof(event.TradeTime), field(row, "time"));
    std::string flag = field(row, "trade_flag");
    if (flag.empty()) {
        flag = field(row, "order_type");
    }
    copy_text(event.OrderKind, sizeof(event.OrderKind), flag);
    event.Price = as_double(field(row, "price"));
    event.Volume = as_double(field(row, "quantity"));
    event.BidApplSeqNum = as_int64(field(row, "bid_id"));
    event.OfferApplSeqNum = as_int64(field(row, "ask_id"));
    event.ApplSeqNum = as_int64(field(row, "sequence"));
    event.BizIndex = event.ApplSeqNum;
    event.IsLast = 1;
    return event;
}

LFL2MarketDataField make_observation(const CsvRow& row) {
    LFL2MarketDataField event;
    std::memset(&event, 0, sizeof(event));
    copy_text(event.InstrumentID, sizeof(event.InstrumentID), field(row, "instrument"));
    copy_text(event.ExchangeID, sizeof(event.ExchangeID), "SZ");
    copy_text(event.TimeStamp, sizeof(event.TimeStamp), field(row, "time"));
    event.LastPrice = as_double(field(row, "last_price"));
    event.TotalTradeVolume = as_double(field(row, "total_volume"));
    event.TotalTradeValue = as_double(field(row, "turnover"));
    event.BidPrice1 = as_double(field(row, "bid_price"));
    event.BidVolume1 = as_double(field(row, "bid_volume"));
    event.OfferPrice1 = as_double(field(row, "ask_price"));
    event.OfferVolume1 = as_double(field(row, "ask_volume"));
    event.UpperLimitPrice = as_double(field(row, "upper_price"));
    event.LowerLimitPrice = as_double(field(row, "lower_price"));
    return event;
}

const char* block_reason_text(sz_hp::SampleBlockReason reason) {
    switch (reason) {
        case sz_hp::SampleBlockReason::kNone: return "none";
        case sz_hp::SampleBlockReason::kUnavailable: return "unavailable";
        case sz_hp::SampleBlockReason::kInvalidObservation: return "invalid_observation";
        case sz_hp::SampleBlockReason::kFirstObservation: return "first_observation";
        case sz_hp::SampleBlockReason::kMarketDataInvalid: return "market_data_invalid";
        case sz_hp::SampleBlockReason::kBeforeOpen: return "before_open";
        case sz_hp::SampleBlockReason::kNoCurrentObservation: return "no_current_observation";
        case sz_hp::SampleBlockReason::kTurnoverIncomplete: return "turnover_incomplete";
        case sz_hp::SampleBlockReason::kTriggerNotMet: return "trigger_not_met";
        case sz_hp::SampleBlockReason::kInsufficientAsk: return "insufficient_ask";
        case sz_hp::SampleBlockReason::kInsufficientVolume: return "insufficient_volume";
        case sz_hp::SampleBlockReason::kSameMillisecond: return "same_millisecond";
        case sz_hp::SampleBlockReason::kAlreadyPending: return "already_pending";
        case sz_hp::SampleBlockReason::kAdapterRejected: return "adapter_rejected";
    }
    return "unknown";
}

const char* diagnostic_code_text(sz_hp::AdapterDiagnostic::Code code) {
    switch (code) {
        case sz_hp::AdapterDiagnostic::kNone: return "none";
        case sz_hp::AdapterDiagnostic::kNullInput: return "null_input";
        case sz_hp::AdapterDiagnostic::kInvalidInstrument: return "invalid_instrument";
        case sz_hp::AdapterDiagnostic::kInvalidSequence: return "invalid_sequence";
        case sz_hp::AdapterDiagnostic::kInvalidTime: return "invalid_time";
        case sz_hp::AdapterDiagnostic::kInvalidQuantity: return "invalid_quantity";
        case sz_hp::AdapterDiagnostic::kBookFailure: return "book_failure";
    }
    return "unknown";
}

std::vector<float> to_vector(const std::array<float, sz_hp::kHpCobFactorCount>& values) {
    return std::vector<float>(values.begin(), values.end());
}

class RuntimeReplay {
public:
    RuntimeReplay() : states_(), config_() {
        config_.capture_failure_digest = true;
        states_.reserve(64);
    }

    sz_hp::InstrumentState* state(const std::string& instrument) {
        StateMap::iterator it = states_.find(instrument);
        if (it != states_.end()) {
            return it->second.get();
        }
        std::unique_ptr<sz_hp::InstrumentState> value(
            new sz_hp::InstrumentState(instrument, config_));
        sz_hp::InstrumentState* result = value.get();
        states_.insert(std::make_pair(instrument, std::move(value)));
        return result;
    }

private:
    typedef std::unordered_map<std::string, std::unique_ptr<sz_hp::InstrumentState> > StateMap;
    StateMap states_;
    sz_hp::SamplerConfig config_;
};

bool emit_event(RuntimeReplay* replay,
                const CsvRow& row,
                uint64_t event_index,
                std::ostream* output) {
    if (replay == 0 || output == 0) {
        return false;
    }
    const std::string type = field(row, "event");
    const std::string instrument = field(row, "instrument");
    sz_hp::InstrumentState* state = replay->state(instrument);
    const std::string before_digest = state->digest();
    sz_hp::AdapterDiagnostic diagnostic;
    sz_hp::SampleDecision decision;
    bool adapter_ok = false;
    bool mutation_ok = false;

    if (type == "order") {
        const LFL2OrderField source = make_order(row);
        sz_hp::OrderEvent event;
        adapter_ok = sz_hp::EventAdapter::normalize_order(
            source, &event, &diagnostic, event_index);
        mutation_ok = adapter_ok && state->process_order(event);
        if (adapter_ok && !mutation_ok) {
            diagnostic.code = sz_hp::AdapterDiagnostic::kBookFailure;
            diagnostic.sequence = static_cast<int64_t>(event.sequence);
            diagnostic.reason = state->book().failure_reason();
        }
    } else if (type == "trade") {
        const LFL2TradeField source = make_trade(row);
        sz_hp::TradeEvent event;
        adapter_ok = sz_hp::EventAdapter::normalize_trade(
            source, &event, &diagnostic, event_index);
        if (adapter_ok) {
            decision = state->process_trade(event);
            mutation_ok = state->available();
            if (!mutation_ok) {
                diagnostic.code = sz_hp::AdapterDiagnostic::kBookFailure;
                diagnostic.sequence = static_cast<int64_t>(event.sequence);
                diagnostic.bid_id = static_cast<int64_t>(event.bid_id);
                diagnostic.ask_id = static_cast<int64_t>(event.ask_id);
                diagnostic.reason = state->book().failure_reason();
            }
        }
    } else if (type == "observation" || type == "snapshot") {
        const LFL2MarketDataField source = make_observation(row);
        sz_hp::MarketObservation event;
        adapter_ok = sz_hp::EventAdapter::normalize_observation(
            source, &event, &diagnostic, event_index);
        if (adapter_ok) {
            decision = state->process_observation(event);
            mutation_ok = state->available();
        }
    } else {
        diagnostic.reason = "unknown fixture event";
    }
    if (!adapter_ok && state->available()) {
        const int64_t source_sequence = as_int64(field(row, "sequence"));
        state->reject_event(source_sequence > 0
                                ? static_cast<uint64_t>(source_sequence)
                                : event_index,
                            diagnostic.reason.c_str());
    }

    nlohmann::json record;
    record["schema"] = "sz-hp-replay-event-v1";
    record["event_index"] = event_index;
    record["event"] = type;
    record["instrument"] = instrument;
    record["sequence"] = as_int64(field(row, "sequence"));
    record["adapter_ok"] = adapter_ok;
    record["mutation_ok"] = mutation_ok;
    record["available"] = state->available();
    record["digest"] = state->digest();
    record["mutation_applied"] = before_digest != state->digest();
    record["prediction_suppressed"] = !state->available();
    record["pre_failure_digest"] = state->pre_failure_digest();
    nlohmann::json diagnostic_json;
    diagnostic_json["code"] = diagnostic_code_text(diagnostic.code);
    diagnostic_json["instrument"] = std::string(diagnostic.instrument.data());
    diagnostic_json["event_index"] = diagnostic.event_index;
    diagnostic_json["sequence"] = diagnostic.sequence;
    diagnostic_json["bid_id"] = diagnostic.bid_id;
    diagnostic_json["ask_id"] = diagnostic.ask_id;
    diagnostic_json["reason"] = diagnostic.reason;
    record["diagnostic"] = diagnostic_json;
    record["sample_ready"] = decision.ready;
    record["sample_reason"] = block_reason_text(decision.reason);
    record["sample_sequence"] = decision.sequence;
    record["sample_time_ms"] = decision.event_time_ms;

    if (decision.ready) {
        sz_hp::SampleBatch batch;
        if (!state->consume_sample(&batch)) {
            record["sample_error"] = "consume_failed";
        } else {
            const sz_hp::FactorInput factors = sz_hp::build_factor_input(*state, batch);
            record["sample_index"] = batch.sample_index;
            record["factor_valid"] = factors.valid;
            record["factor_raw"] = to_vector(factors.cob_values.values);
            record["factor_model_order"] = to_vector(factors.cob_values.model_values);
            record["prediction_status"] = "missing_hp_model_artifact";
        }
    }
    *output << record.dump() << '\n';
    return adapter_ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sz_hp_replay FIXTURE.csv\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    if (!input.is_open()) {
        std::cerr << "cannot open fixture: " << argv[1] << "\n";
        return 2;
    }

    nlohmann::json metadata;
    metadata["schema"] = "sz-hp-replay-v1";
    metadata["source"] = "runtime";
    metadata["fixture"] = argv[1];
    metadata["factor_count"] = sz_hp::kHpCobFactorCount;
    metadata["factor_names"] = std::vector<const char*>(
        sz_hp::hp_cob_factor_names().begin(), sz_hp::hp_cob_factor_names().end());
    metadata["model_input_names"] = std::vector<const char*>(
        sz_hp::hp_cob_model_input_names().begin(),
        sz_hp::hp_cob_model_input_names().end());
    metadata["prediction_status"] = "missing_hp_model_artifact";
    std::cout << metadata.dump() << '\n';

    RuntimeReplay replay;
    std::vector<std::string> header;
    std::string line;
    uint64_t event_index = 0;
    bool all_adapters_ok = true;
    while (std::getline(input, line)) {
        const std::string cleaned = trim(line);
        if (cleaned.empty()) {
            continue;
        }
        if (cleaned[0] == '#') {
            const std::string candidate = trim(cleaned.substr(1));
            if (candidate.find("event,") == 0) {
                header = split_csv(candidate);
                for (size_t i = 0; i < header.size(); ++i) {
                    header[i] = trim(header[i]);
                }
            }
            continue;
        }
        if (header.empty()) {
            std::cerr << "fixture has no '# event,...' header\n";
            return 2;
        }
        const std::vector<std::string> values = split_csv(cleaned);
        CsvRow row;
        for (size_t i = 0; i < header.size(); ++i) {
            row[header[i]] = i < values.size() ? trim(values[i]) : std::string();
        }
        ++event_index;
        all_adapters_ok = emit_event(&replay, row, event_index, &std::cout) &&
                          all_adapters_ok;
    }
    return all_adapters_ok ? 0 : 1;
}
