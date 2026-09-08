#include "auction_static_metadata.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace sse_auction59 {
namespace {

void fail(const std::string& message, std::string* error) {
    if (error) *error = message;
}

// Mirror of sse_live::is_sse_stock (sse/market_data/sse_primary_decoder.cpp).
// Kept local so the Auction59 package has no link dependency on the decoder;
// keep the two predicates in sync if the decoder rule ever changes.
bool is_sse_stock_id(const std::string& security_id) {
    return security_id.size() == 6U &&
           ((security_id[0] == '6' && security_id[1] == '0') ||
            (security_id[0] == '6' && security_id[1] == '8'));
}

bool parse_csv_line(const std::string& line, std::vector<std::string>* fields) {
    fields->clear();
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char value = line[i];
        if (quoted) {
            if (value == '"') {
                if (i + 1U < line.size() && line[i + 1U] == '"') {
                    field.push_back('"'); ++i;
                } else quoted = false;
            } else field.push_back(value);
        } else if (value == ',' ) {
            fields->push_back(field); field.clear();
        } else if (value == '"' && field.empty()) quoted = true;
        else field.push_back(value);
    }
    if (quoted) return false;
    fields->push_back(field);
    return true;
}

bool u32_field(const std::string& text, std::uint32_t* value) {
    if (text.empty()) { *value = 0U; return true; }
    char* end = 0;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (!end || end == text.c_str() || *end != '\0' || parsed > 0xffffffffUL) return false;
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool double_field(const std::string& text, double* value) {
    if (text.empty()) { *value = 0.0; return true; }
    char* end = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (!end || end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

bool bool_field(const std::string& text, bool* value) {
    if (text == "0") { *value = false; return true; }
    if (text == "1") { *value = true; return true; }
    return false;
}

}  // namespace

StaticMetadata::StaticMetadata()
    : date(0), pre_close(0.0), upper_limit(0.0), lower_limit(0.0),
      listing_date(0), is_ipo_first_day(false), limits_valid(false) {}

bool StaticMetadata::auction_static_ready() const {
    return pre_close > 0.0 && upper_limit > 0.0 && lower_limit > 0.0 &&
           upper_limit >= lower_limit && limits_valid;
}

bool load_static_metadata_csv(const std::string& path, std::uint32_t expected_date,
                              StaticMetadataMap* output, std::string* error) {
    if (error) error->clear();
    if (!output) { fail("null static metadata output", error); return false; }
    output->clear();
    std::ifstream input(path.c_str());
    if (!input) { fail("cannot open static metadata CSV: " + path, error); return false; }
    const char* expected[] = {"security_id", "name", "date", "pre_close", "upper_limit",
                              "lower_limit", "listing_date", "is_ipo_first_day", "limits_valid",
                              "source", "quality", "error"};
    std::string line;
    std::vector<std::string> fields;
    if (!std::getline(input, line)) {
        fail("invalid static metadata header", error); return false;
    }
    if (!line.empty() && line[line.size() - 1U] == '\r') line.erase(line.size() - 1U);
    if (line.size() >= 3U && static_cast<unsigned char>(line[0]) == 0xefU &&
        static_cast<unsigned char>(line[1]) == 0xbbU &&
        static_cast<unsigned char>(line[2]) == 0xbfU) line.erase(0, 3);
    if (!parse_csv_line(line, &fields) || fields.size() != 12U) {
        fail("invalid static metadata header", error); return false;
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i] != expected[i]) { fail("unexpected static metadata column", error); return false; }
    }
    std::size_t line_no = 1U;
    while (std::getline(input, line)) {
        ++line_no;
        if (!line.empty() && line[line.size() - 1U] == '\r') line.erase(line.size() - 1U);
        if (line.empty()) continue;
        if (!parse_csv_line(line, &fields) || fields.size() != 12U) {
            fail("invalid static metadata CSV row", error); return false;
        }
        if (!is_sse_stock_id(fields[0]) || output->find(fields[0]) != output->end()) {
            fail("invalid or duplicate static metadata security: " + fields[0], error); return false;
        }
        StaticMetadata value;
        if (!u32_field(fields[2], &value.date) || value.date != expected_date ||
            !double_field(fields[3], &value.pre_close) ||
            !double_field(fields[4], &value.upper_limit) ||
            !double_field(fields[5], &value.lower_limit) ||
            !u32_field(fields[6], &value.listing_date) ||
            !bool_field(fields[7], &value.is_ipo_first_day) ||
            !bool_field(fields[8], &value.limits_valid)) {
            fail("invalid static metadata values on line " + std::to_string(line_no), error);
            return false;
        }
        if (value.is_ipo_first_day != (value.listing_date == value.date) ||
            value.limits_valid != (value.pre_close > 0.0 && value.upper_limit > 0.0 &&
                                   value.lower_limit > 0.0 && value.upper_limit >= value.lower_limit)) {
            fail("inconsistent static metadata flags for " + fields[0], error); return false;
        }
        value.source = fields[9]; value.quality = fields[10];
        (*output)[fields[0]] = value;
    }
    return true;
}

bool reconcile_snapshot_pre_close(const sse_live::Snapshot& snapshot,
                                  const StaticMetadataMap& metadata,
                                  std::string* quality) {
    StaticMetadataMap::const_iterator found = metadata.find(snapshot.security_id);
    if (found == metadata.end()) {
        if (quality) *quality = "static_security_missing";
        return false;
    }
    if (found->second.pre_close <= 0.0 || snapshot.pre_close_price <= 0.0) {
        if (quality) *quality = "pre_close_missing";
        return false;
    }
    if (std::fabs(found->second.pre_close - snapshot.pre_close_price) > 0.0005) {
        if (quality) *quality = "pre_close_conflict";
        return false;
    }
    if (!found->second.auction_static_ready()) {
        if (quality) *quality = "explicit_limits_missing";
        return false;
    }
    if (quality) *quality = "ok";
    return true;
}

}  // namespace sse_auction59
