#include "third_party/nlohmann/json.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using nlohmann::json;

namespace {

const char* kRequiredFields[] = {
    "Close", "FreeShare", "HistoryAmount", "HistoryVolatility20d",
    "HpLowerPrice", "HpUpperPrice", "Date", "static_position"};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint32_t require_day(const json& object, const char* key) {
    const json::const_iterator it = object.find(key);
    if (it == object.end() || !it->is_number_unsigned() ||
        it->get<std::uint64_t>() < 20000101ULL ||
        it->get<std::uint64_t>() > 99991231ULL) {
        fail(std::string("invalid ") + key);
    }
    return it->get<std::uint32_t>();
}

double require_number(const json& object, const char* key, bool positive) {
    const json::const_iterator it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        fail(std::string("missing numeric field ") + key);
    }
    const double value = it->get<double>();
    if (!std::isfinite(value) || (positive && value <= 0.0)) {
        fail(std::string("invalid numeric field ") + key);
    }
    return value;
}

std::string day_string(std::uint32_t day) {
    std::ostringstream out;
    out << std::setfill('0') << std::setw(8) << day;
    return out.str();
}

void validate_daily(const json& daily, std::uint32_t expected_day) {
    if (!daily.is_object()) fail("daily config must be a JSON object");
    const std::uint32_t trading_day = require_day(daily, "trading_day");
    if (trading_day != expected_day) fail("trading_day does not match requested date");
    (void)require_day(daily, "static_data_source_date");
    const json::const_iterator params = daily.find("ins_params");
    if (params == daily.end() || !params->is_object() || params->empty()) {
        fail("ins_params must be a non-empty object");
    }
    std::size_t checked = 0;
    for (json::const_iterator item = params->begin(); item != params->end(); ++item) {
        const std::string symbol = item.key();
        const bool valid_symbol = symbol.size() == 9U &&
            (symbol.compare(0, 2, "60") == 0 || symbol.compare(0, 2, "68") == 0) &&
            symbol[6] == '.' && symbol.compare(7, 2, "SH") == 0 &&
            symbol[2] >= '0' && symbol[2] <= '9' &&
            symbol[3] >= '0' && symbol[3] <= '9' &&
            symbol[4] >= '0' && symbol[4] <= '9' &&
            symbol[5] >= '0' && symbol[5] <= '9';
        if (!valid_symbol) {
            fail("non-SSE-stock instrument: " + symbol);
        }
        if (!item->is_object()) fail("instrument params must be objects: " + symbol);
        for (const char* field : kRequiredFields) {
            if (item->find(field) == item->end()) {
                fail("missing " + std::string(field) + " for " + symbol);
            }
        }
        if (require_day(*item, "Date") != trading_day) {
            fail("instrument Date mismatch: " + symbol);
        }
        const double close = require_number(*item, "Close", true);
        const double free_share = require_number(*item, "FreeShare", true);
        const double history_amount = require_number(*item, "HistoryAmount", true);
        const double volatility = require_number(*item, "HistoryVolatility20d", false);
        const double lower = require_number(*item, "HpLowerPrice", true);
        const double upper = require_number(*item, "HpUpperPrice", true);
        const double position = require_number(*item, "static_position", false);
        if (volatility < 0.0 || lower > upper || position < 0.0 ||
            std::floor(position) != position) {
            fail("invalid limits/volatility/position for " + symbol);
        }
        (void)close;
        (void)free_share;
        (void)history_amount;
        ++checked;
    }
    if (checked == 0) fail("no instruments survived validation");
}

json build_runtime(const json& daily, const std::string& output_path) {
    const std::uint32_t day = daily.at("trading_day").get<std::uint32_t>();
    const std::string day_text = day_string(day);
    json runtime = json::object();
    runtime["name"] = "HStrategy";
    runtime["strategy_name"] = std::string("sse_t0_daily_") + day_text;
    runtime["market"] = "SH";
    runtime["trading_day"] = day;
    runtime["static_data_source_date"] = daily.at("static_data_source_date");
    runtime["runtime_mode"] = "staged";
    runtime["prediction_only"] = true;
    runtime["trading_enabled"] = false;
    runtime["production_approval"] = false;
    runtime["md_source_index"] = json::array({89});
    runtime["td_source_index"] = json::array({190});
    runtime["instrument_universe_mode"] = "all-sse-equities-60-68";
    runtime["sh_orderbook_mode"] = "full-orderbook";
    runtime["ins_params"] = daily.at("ins_params");
    std::size_t nonzero_positions = 0U;
    for (json::const_iterator item = daily.at("ins_params").begin();
         item != daily.at("ins_params").end(); ++item) {
        if (item->at("static_position").get<double>() > 0.0) ++nonzero_positions;
    }
    runtime["static_position_nonzero_count"] = nonzero_positions;
    runtime["position_reconciliation"] = {
        {"required_before_trading", true},
        {"account_query_source", 190},
        {"note", "Do not treat static_position as live available position until TD position query matches."}
    };
    runtime["sse_order_routing"] = {
        {"enabled", false},
        {"mode", "staged"},
        {"td_source", 190},
        {"position_query_retry_ms", 5000},
        {"position_query_cutoff_hhmmss", 93100}
    };
    runtime["sse_activation"] = {
        {"requires_production_approval", true},
        {"requires_trade_password", true},
        {"startup_cancel_all_orders", false},
        {"activation_note", "Set trading_enabled, production_approval and routing.enabled only after explicit pre-open approval."}
    };
    runtime["daily_source"] = {
        {"config_path", output_path},
        {"static_data_hash", daily.value("static_data_hash", std::string())}
    };
    return runtime;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: sse_daily_config_builder DAILY_JSON OUTPUT_JSON [YYYYMMDD]\\n";
        return 2;
    }
    try {
        std::ifstream input(argv[1]);
        if (!input) fail(std::string("cannot open ") + argv[1]);
        json daily;
        input >> daily;
        const std::uint32_t day = argc == 4
            ? static_cast<std::uint32_t>(std::stoul(argv[3]))
            : daily.at("trading_day").get<std::uint32_t>();
        validate_daily(daily, day);
        const json runtime = build_runtime(daily, argv[2]);
        std::ofstream output(argv[2]);
        if (!output) fail(std::string("cannot open output ") + argv[2]);
        output << runtime.dump(2) << '\n';
        std::cout << "validated_instruments=" << daily.at("ins_params").size()
                  << " trading_day=" << day << " output=" << argv[2] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sse_daily_config_builder: " << error.what() << '\n';
        return 1;
    }
}
