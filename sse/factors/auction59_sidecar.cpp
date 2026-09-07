#include "sse/factors/auction59_sidecar.h"

#include "sse/market_data/sse_primary_decoder.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sse_auction59 {
namespace {

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream input(value);
    while (std::getline(input, field, delimiter)) fields.push_back(field);
    if (!value.empty() && value[value.size() - 1U] == delimiter) fields.push_back(std::string());
    return fields;
}

void fail(const std::string& message, std::string* error) {
    if (error) *error = message;
}

}  // namespace

bool load_csv(const std::string& path, FactorMap* output, std::string* error) {
    if (error) error->clear();
    if (!output) { fail("null Auction59 output", error); return false; }
    output->clear();
    if (path.empty()) return true;
    std::ifstream input(path.c_str());
    if (!input) { fail("cannot open Auction59 CSV: " + path, error); return false; }
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        if (line.empty()) continue;
        const std::vector<std::string> fields = split(line, ',');
        const bool header = line_no == 1U && !fields.empty() &&
                            (fields[0] == "security_id" || fields[0] == "SecurityID");
        if (header) {
            if (fields.size() != 60U) {
                fail("Auction59 CSV header must have 60 fields", error); return false;
            }
            continue;
        }
        if (fields.size() != 60U) {
            std::ostringstream message;
            message << "Auction59 CSV line " << line_no << " must have 60 fields";
            fail(message.str(), error); return false;
        }
        if (!sse_live::is_sse_stock(fields[0])) {
            fail("invalid Auction59 security: " + fields[0], error); return false;
        }
        if (output->find(fields[0]) != output->end()) {
            fail("duplicate Auction59 security: " + fields[0], error); return false;
        }
        std::vector<float> factors(59U);
        for (std::size_t i = 0; i < factors.size(); ++i) {
            char* end = 0;
            factors[i] = std::strtof(fields[i + 1U].c_str(), &end);
            if (!end || end == fields[i + 1U].c_str() || *end != '\0' ||
                std::isinf(factors[i])) {
                fail("invalid Auction59 value for " + fields[0], error); return false;
            }
        }
        (*output)[fields[0]] = factors;
    }
    return true;
}

}  // namespace sse_auction59
