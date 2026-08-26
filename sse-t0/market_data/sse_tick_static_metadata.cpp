#include "sse_tick_static_metadata.h"

#include "sse_primary_decoder.h"
#include "../../src/t0-main/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <vector>

namespace sse_tick {
namespace {

void fail(const std::string& message, std::string* error) {
    if (error) *error = message;
}

std::string trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1U]))) --end;
    return value.substr(begin, end - begin);
}

std::string lower(std::string value) {
    for (std::size_t i = 0; i < value.size(); ++i)
        value[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
    return value;
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
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(value);
            }
        } else if (value == ',') {
            fields->push_back(trim(field)); field.clear();
        } else if (value == '"' && field.empty()) {
            quoted = true;
        } else {
            field.push_back(value);
        }
    }
    if (quoted) return false;
    fields->push_back(trim(field));
    return true;
}

void strip_bom(std::string* value) {
    if (value->size() >= 3U &&
        static_cast<unsigned char>((*value)[0]) == 0xefU &&
        static_cast<unsigned char>((*value)[1]) == 0xbbU &&
        static_cast<unsigned char>((*value)[2]) == 0xbfU)
        value->erase(0, 3);
}

bool parse_u32(const std::string& text, std::uint32_t* value, bool* present) {
    const std::string clean = trim(text);
    if (clean.empty()) { *present = false; *value = 0U; return true; }
    char* end = 0;
    const unsigned long parsed = std::strtoul(clean.c_str(), &end, 10);
    if (!end || end == clean.c_str() || *end != '\0' || parsed > 0xffffffffUL)
        return false;
    *present = true;
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_double(const std::string& text, double* value, bool* present) {
    const std::string clean = trim(text);
    if (clean.empty()) { *present = false; *value = 0.0; return true; }
    char* end = 0;
    const double parsed = std::strtod(clean.c_str(), &end);
    if (!end || end == clean.c_str() || *end != '\0' || !std::isfinite(parsed))
        return false;
    *present = true;
    *value = parsed;
    return true;
}

std::size_t find_column(const std::vector<std::string>& header,
                        const char* first, const char* second = 0,
                        const char* third = 0) {
    for (std::size_t i = 0; i < header.size(); ++i) {
        const std::string name = lower(trim(header[i]));
        if (name == first || (second && name == second) ||
            (third && name == third)) return i;
    }
    return header.size();
}

std::string cell(const std::vector<std::string>& fields, std::size_t column) {
    return column < fields.size() ? fields[column] : std::string();
}

bool suffix_case(const std::string& value, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return value.size() >= n && value.compare(value.size() - n, n, suffix) == 0;
}

bool json_member(const nlohmann::json& object, const char* name,
                 const nlohmann::json** value, std::string* error) {
    if (!object.is_object()) {
        fail("daily static metadata JSON member lookup requires an object", error);
        return false;
    }
    nlohmann::json::const_iterator item = object.find(name);
    if (item == object.end()) {
        fail(std::string("daily static metadata JSON missing field: ") + name, error);
        return false;
    }
    *value = &(*item);
    return true;
}

bool json_string(const nlohmann::json& object, const char* name,
                 std::string* value, std::string* error) {
    const nlohmann::json* item = 0;
    if (!json_member(object, name, &item, error)) return false;
    if (!item->is_string()) {
        fail(std::string("daily static metadata JSON field is not a string: ") + name,
             error);
        return false;
    }
    *value = item->get<std::string>();
    return true;
}

bool json_u32(const nlohmann::json& object, const char* name,
              std::uint32_t* value, std::string* error) {
    const nlohmann::json* item = 0;
    if (!json_member(object, name, &item, error)) return false;
    if (!item->is_number_integer()) {
        fail(std::string("daily static metadata JSON field is not an integer: ") + name,
             error);
        return false;
    }
    try {
        const long long parsed = item->get<long long>();
        if (parsed < 0LL || parsed > 0xffffffffLL) {
            fail(std::string("daily static metadata JSON integer is out of range: ") + name,
                 error);
            return false;
        }
        *value = static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        fail(std::string("daily static metadata JSON integer is invalid: ") + name, error);
        return false;
    }
    return true;
}

bool json_double(const nlohmann::json& object, const char* name,
                 double* value, std::string* error) {
    const nlohmann::json* item = 0;
    if (!json_member(object, name, &item, error)) return false;
    if (!item->is_number()) {
        fail(std::string("daily static metadata JSON field is not numeric: ") + name,
             error);
        return false;
    }
    try {
        *value = item->get<double>();
    } catch (const std::exception&) {
        fail(std::string("daily static metadata JSON number is invalid: ") + name, error);
        return false;
    }
    if (!std::isfinite(*value)) {
        fail(std::string("daily static metadata JSON number is not finite: ") + name,
             error);
        return false;
    }
    return true;
}

bool valid_sha256(const std::string& value) {
    if (value.size() != 64U) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// Small dependency-free SHA-256 implementation used only for validating the
// source files named by the normalized JSON envelope.  This keeps the live
// C++11 build independent of OpenSSL/pyarrow/Node.
class Sha256 {
public:
    Sha256() : length_(0U), block_size_(0U) {
        state_[0] = 0x6a09e667U; state_[1] = 0xbb67ae85U;
        state_[2] = 0x3c6ef372U; state_[3] = 0xa54ff53aU;
        state_[4] = 0x510e527fU; state_[5] = 0x9b05688cU;
        state_[6] = 0x1f83d9abU; state_[7] = 0x5be0cd19U;
    }

    void update(const unsigned char* data, std::size_t size) {
        length_ += static_cast<std::uint64_t>(size);
        for (std::size_t i = 0; i < size; ++i) {
            block_[block_size_++] = data[i];
            if (block_size_ == sizeof(block_)) {
                transform(block_);
                block_size_ = 0U;
            }
        }
    }

    std::string finish() {
        const std::uint64_t bit_length = length_ * 8ULL;
        block_[block_size_++] = 0x80U;
        while (block_size_ != 56U) {
            if (block_size_ == sizeof(block_)) {
                transform(block_);
                block_size_ = 0U;
            }
            block_[block_size_++] = 0U;
        }
        for (int shift = 56; shift >= 0; shift -= 8)
            block_[block_size_++] = static_cast<unsigned char>(bit_length >> shift);
        transform(block_);

        static const char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64U);
        for (std::size_t i = 0; i < 8U; ++i) {
            for (int shift = 28; shift >= 0; shift -= 4)
                result.push_back(hex[(state_[i] >> shift) & 0x0fU]);
        }
        return result;
    }

private:
    static std::uint32_t rotr(std::uint32_t value, unsigned int bits) {
        return (value >> bits) | (value << (32U - bits));
    }

    void transform(const unsigned char* block) {
        static const std::uint32_t constants[64] = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
        };
        std::uint32_t words[64];
        for (std::size_t i = 0; i < 16U; ++i) {
            const std::size_t offset = i * 4U;
            words[i] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                       (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                       (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                       static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t i = 16U; i < 64U; ++i) {
            const std::uint32_t s0 = rotr(words[i - 15U], 7U) ^
                rotr(words[i - 15U], 18U) ^ (words[i - 15U] >> 3U);
            const std::uint32_t s1 = rotr(words[i - 2U], 17U) ^
                rotr(words[i - 2U], 19U) ^ (words[i - 2U] >> 10U);
            words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (std::size_t i = 0; i < 64U; ++i) {
            const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + choose + constants[i] + words[i];
            const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::uint64_t length_;
    std::size_t block_size_;
    unsigned char block_[64];
    std::uint32_t state_[8];
};

bool sha256_file(const std::string& path, std::string* digest) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return false;
    Sha256 sha;
    char buffer[16384];
    while (input.good()) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0)
            sha.update(reinterpret_cast<const unsigned char*>(buffer),
                       static_cast<std::size_t>(count));
    }
    if (!input.eof()) return false;
    *digest = sha.finish();
    return true;
}

bool verify_json_source_hashes(const nlohmann::json& source, std::string* error) {
    const char* hash_names[] = {
        "config_sha256", "config_static_data_hash", "manifest_sha256", "stock_day_sha256"
    };
    for (std::size_t i = 0; i < sizeof(hash_names) / sizeof(hash_names[0]); ++i) {
        std::string value;
        if (!json_string(source, hash_names[i], &value, error)) return false;
        if (!valid_sha256(value)) {
            fail(std::string("daily static metadata JSON hash is invalid: ") + hash_names[i],
                 error);
            return false;
        }
    }
    const char* path_names[] = {"config_path", "manifest_path", "stock_day_path"};
    const char* expected_names[] = {"config_sha256", "manifest_sha256", "stock_day_sha256"};
    for (std::size_t i = 0; i < sizeof(path_names) / sizeof(path_names[0]); ++i) {
        nlohmann::json::const_iterator path_item = source.find(path_names[i]);
        if (path_item == source.end()) continue;
        if (!path_item->is_string()) {
            fail(std::string("daily static metadata JSON source path is not a string: ") +
                     path_names[i], error);
            return false;
        }
        std::string expected;
        if (!json_string(source, expected_names[i], &expected, error)) return false;
        std::string actual;
        if (!sha256_file(path_item->get<std::string>(), &actual)) {
            fail(std::string("daily static metadata JSON source file is unreadable: ") +
                     path_item->get<std::string>(), error);
            return false;
        }
        if (lower(actual) != lower(expected)) {
            fail(std::string("daily static metadata JSON source hash mismatch: ") +
                     path_names[i], error);
            return false;
        }
    }
    return true;
}

bool json_record_number(const nlohmann::json& record, const char* name,
                        double* value, std::string* error) {
    if (!json_double(record, name, value, error)) return false;
    if (*value <= 0.0 && (std::strcmp(name, "prev_turnover") != 0)) {
        fail(std::string("daily static metadata JSON field is not positive: ") + name,
             error);
        return false;
    }
    if (std::strcmp(name, "prev_turnover") == 0 && *value < 0.0) {
        fail("daily static metadata JSON prev_turnover is negative", error);
        return false;
    }
    return true;
}

// The research-machine config is the authoritative daily source.  Its
// HistoryAmount field is the model's amount baseline; the old CSV sidecar
// duplicated it as avg_amount and supplied turnover_threshold separately.
// Keep the derivation here so the live/recovery path has one input artifact.
bool load_source_daily_config(const nlohmann::json& document,
                              std::uint32_t expected_date,
                              DailyStaticMetadataMap* output,
                              std::string* error) {
    std::uint32_t trading_day = 0U;
    if (!json_u32(document, "trading_day", &trading_day, error) ||
        trading_day == 0U) {
        fail("daily config JSON trading_day is invalid", error);
        return false;
    }
    if (expected_date != 0U && trading_day != expected_date) {
        fail("daily config JSON trading_day mismatch", error);
        return false;
    }
    if (expected_date == 0U) expected_date = trading_day;

    const nlohmann::json* params = 0;
    if (!json_member(document, "ins_params", &params, error) ||
        !params->is_object() || params->empty()) {
        fail("daily config JSON ins_params must be a non-empty object", error);
        return false;
    }

    for (nlohmann::json::const_iterator item = params->begin();
         item != params->end(); ++item) {
        std::string security;
        if (!normalize_sse_security_id(item.key(), &security)) {
            fail("invalid or non-SSE security in daily config JSON: " + item.key(),
                 error);
            return false;
        }
        if (!item->is_object()) {
            fail("daily config JSON instrument is not an object: " + security, error);
            return false;
        }
        if (output->find(security) != output->end()) {
            fail("duplicate daily config JSON security: " + security, error);
            return false;
        }

        DailyStaticMetadata metadata;
        if (!json_u32(*item, "Date", &metadata.date, error) ||
            metadata.date != expected_date) {
            fail("daily config JSON Date mismatch for " + security, error);
            return false;
        }
        metadata.has_date = true;
        if (!json_record_number(*item, "HistoryAmount", &metadata.avg_amount, error) ||
            !json_record_number(*item, "FreeShare", &metadata.free_share, error) ||
            !json_record_number(*item, "Close", &metadata.pre_close, error) ||
            !json_record_number(*item, "HpUpperPrice", &metadata.limit_price, error) ||
            !json_record_number(*item, "HpLowerPrice", &metadata.stop_price, error)) {
            return false;
        }
        metadata.turnover_threshold = metadata.avg_amount / 8000.0;
        if (!std::isfinite(metadata.turnover_threshold) ||
            metadata.turnover_threshold <= 0.0) {
            fail("daily config JSON derived turnover_threshold is invalid for " + security,
                 error);
            return false;
        }
        metadata.has_avg_amount = true;
        metadata.has_turnover_threshold = true;
        metadata.has_free_share = true;
        metadata.has_pre_close = true;
        metadata.has_limit_price = true;
        metadata.has_stop_price = true;
        metadata.threshold_basis = "HistoryAmount/8000";
        metadata.source = "config_sse_daily";
        metadata.quality = "daily_json";
        if (metadata.limit_price < metadata.stop_price) {
            fail("daily config JSON price bounds are inverted for " + security, error);
            return false;
        }
        (*output)[security] = metadata;
    }
    return !output->empty();
}

}  // namespace

DailyStaticMetadata::DailyStaticMetadata()
    : date(0), prev_trade_date(0), prev_turnover(0.0), avg_amount(0.0),
      turnover_threshold(0.0), free_share(0.0), pre_close(0.0), limit_price(0.0),
      stop_price(0.0), has_date(false), has_prev_trade_date(false),
      has_prev_turnover(false), has_avg_amount(false),
      has_turnover_threshold(false), has_free_share(false), has_pre_close(false),
      has_limit_price(false), has_stop_price(false), threshold_basis(), source(),
      quality() {}

bool DailyStaticMetadata::complete() const {
    return has_date && date != 0U && has_avg_amount && avg_amount > 0.0 &&
           has_turnover_threshold &&
           turnover_threshold > 0.0 && has_free_share && free_share > 0.0 &&
           has_pre_close && pre_close > 0.0 && has_limit_price &&
           limit_price > 0.0 && has_stop_price && stop_price > 0.0 &&
           limit_price >= stop_price &&
           (quality.empty() || (quality != "missing" && quality != "error"));
}

bool normalize_sse_security_id(const std::string& value, std::string* output) {
    if (!output) return false;
    std::string clean = trim(value);
    strip_bom(&clean);
    if (clean.size() >= 2U && clean[0] == '"' &&
        clean[clean.size() - 1U] == '"')
        clean = clean.substr(1U, clean.size() - 2U);
    clean = trim(clean);
    for (std::size_t i = 0; i < clean.size(); ++i)
        clean[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(clean[i])));
    if (suffix_case(clean, ".SH")) clean.erase(clean.size() - 3U);
    else if (suffix_case(clean, "SH")) clean.erase(clean.size() - 2U);
    else if (clean.size() > 3U && clean.compare(0, 3, "SH.") == 0)
        clean.erase(0, 3);
    if (!sse_live::is_sse_stock(clean)) return false;
    *output = clean;
    return true;
}

bool load_daily_static_metadata_csv(const std::string& path,
                                    std::uint32_t expected_date,
                                    DailyStaticMetadataMap* output,
                                    std::string* error) {
    if (error) error->clear();
    if (!output) { fail("null daily static metadata output", error); return false; }
    output->clear();
    std::ifstream input(path.c_str());
    if (!input) {
        fail("cannot open daily static metadata CSV: " + path, error);
        return false;
    }
    std::string line;
    std::vector<std::string> header;
    if (!std::getline(input, line)) {
        fail("invalid daily static metadata header", error); return false;
    }
    if (!line.empty() && line[line.size() - 1U] == '\r') line.erase(line.size() - 1U);
    strip_bom(&line);
    if (!parse_csv_line(line, &header) || header.empty()) {
        fail("invalid daily static metadata header", error); return false;
    }
    const std::size_t id_col = find_column(header, "instrument_id", "security_id", "code");
    const std::size_t ts_code_col = find_column(header, "ts_code");
    const std::size_t date_col = find_column(header, "date", "trade_date", "trading_date");
    if (id_col == header.size() && ts_code_col == header.size()) {
        fail("daily static metadata requires instrument_id/security_id/code/ts_code", error);
        return false;
    }
    const std::size_t actual_id_col = id_col != header.size() ? id_col : ts_code_col;
    const std::size_t prev_trade_date_col = find_column(header, "prev_trade_date");
    const std::size_t prev_turnover_col = find_column(header, "prev_turnover");
    const std::size_t avg_amount_col = find_column(header, "avg_amount", "history_amount");
    const std::size_t threshold_col = find_column(header, "turnover_threshold");
    const std::size_t threshold_basis_col = find_column(header, "threshold_basis");
    const std::size_t free_share_col = find_column(header, "free_share", "freeshares");
    const std::size_t pre_close_col = find_column(header, "pre_close", "preclose", "previous_close");
    const std::size_t limit_col = find_column(header, "limit_price", "upper_limit", "limit_up");
    const std::size_t stop_col = find_column(header, "stop_price", "lower_limit", "limit_down");
    const std::size_t source_col = find_column(header, "source");
    const std::size_t quality_col = find_column(header, "quality");
    std::uint32_t file_date = expected_date;
    std::size_t line_no = 1U;
    while (std::getline(input, line)) {
        ++line_no;
        if (!line.empty() && line[line.size() - 1U] == '\r') line.erase(line.size() - 1U);
        if (trim(line).empty() || trim(line)[0] == '#') continue;
        std::vector<std::string> fields;
        if (!parse_csv_line(line, &fields) || fields.size() != header.size()) {
            fail("invalid daily static metadata CSV row on line " +
                 std::to_string(line_no), error);
            return false;
        }
        std::string security;
        if (!normalize_sse_security_id(cell(fields, actual_id_col), &security)) {
            fail("invalid or non-SSE security on line " + std::to_string(line_no), error);
            return false;
        }
        if (output->find(security) != output->end()) {
            fail("duplicate daily static metadata security: " + security, error);
            return false;
        }
        DailyStaticMetadata value;
        bool ok = true;
        if (date_col != header.size()) ok = parse_u32(cell(fields, date_col), &value.date,
                                                       &value.has_date);
        if (!ok || (expected_date != 0U && value.has_date && value.date != expected_date)) {
            fail("daily static metadata date mismatch on line " + std::to_string(line_no), error);
            return false;
        }
        if (expected_date != 0U && !value.has_date) {
            value.date = expected_date;  // date is supplied by the caller, not fabricated.
            value.has_date = true;
        }
        if (value.has_date) {
            if (file_date == 0U) file_date = value.date;
            if (value.date != file_date) {
                fail("mixed trading dates in daily static metadata CSV", error);
                return false;
            }
        }
        if (prev_trade_date_col != header.size())
            ok = ok && parse_u32(cell(fields, prev_trade_date_col), &value.prev_trade_date,
                                 &value.has_prev_trade_date);
        if (prev_turnover_col != header.size())
            ok = ok && parse_double(cell(fields, prev_turnover_col), &value.prev_turnover,
                                    &value.has_prev_turnover);
        if (avg_amount_col != header.size())
            ok = ok && parse_double(cell(fields, avg_amount_col), &value.avg_amount,
                                    &value.has_avg_amount);
        if (threshold_col != header.size())
            ok = ok && parse_double(cell(fields, threshold_col), &value.turnover_threshold,
                                    &value.has_turnover_threshold);
        if (free_share_col != header.size())
            ok = ok && parse_double(cell(fields, free_share_col), &value.free_share,
                                    &value.has_free_share);
        if (pre_close_col != header.size())
            ok = ok && parse_double(cell(fields, pre_close_col), &value.pre_close,
                                    &value.has_pre_close);
        if (limit_col != header.size())
            ok = ok && parse_double(cell(fields, limit_col), &value.limit_price,
                                    &value.has_limit_price);
        if (stop_col != header.size())
            ok = ok && parse_double(cell(fields, stop_col), &value.stop_price,
                                    &value.has_stop_price);
        if (!ok) {
            fail("invalid daily static metadata number on line " + std::to_string(line_no), error);
            return false;
        }
        if (threshold_basis_col != header.size()) value.threshold_basis = cell(fields, threshold_basis_col);
        if (source_col != header.size()) value.source = cell(fields, source_col);
        if (quality_col != header.size()) value.quality = lower(cell(fields, quality_col));
        (*output)[security] = value;
    }
    if (output->empty()) {
        fail("daily static metadata CSV contains no rows", error); return false;
    }
    return true;
}

bool load_daily_static_metadata_json(const std::string& path,
                                     std::uint32_t expected_date,
                                     DailyStaticMetadataMap* output,
                                     std::string* error) {
    if (error) error->clear();
    if (!output) { fail("null daily static metadata output", error); return false; }
    output->clear();
    std::ifstream input(path.c_str());
    if (!input) {
        fail("cannot open daily static metadata JSON: " + path, error);
        return false;
    }
    try {
        nlohmann::json document;
        input >> document;
        if (!document.is_object()) {
            fail("daily static metadata JSON envelope is not an object", error);
            return false;
        }
        // Prefer the research-machine source contract when present.  The
        // normalized v1 envelope below remains supported for older replays.
        if (document.find("ins_params") != document.end()) {
            return load_source_daily_config(document, expected_date, output, error);
        }
        const nlohmann::json* value = 0;
        if (!json_member(document, "schema_version", &value, error) ||
            !value->is_number_integer() || value->get<int>() != 1) {
            fail("daily static metadata JSON schema_version must be 1", error);
            return false;
        }
        std::string contract;
        if (!json_string(document, "contract", &contract, error) ||
            contract != "sse_tick_daily_static_v1") {
            fail("daily static metadata JSON contract is unsupported", error);
            return false;
        }
        std::uint32_t target_date = 0U;
        if (!json_u32(document, "target_date", &target_date, error) ||
            target_date == 0U) {
            fail("daily static metadata JSON target_date is invalid", error);
            return false;
        }
        if (expected_date != 0U && target_date != expected_date) {
            fail("daily static metadata JSON target_date mismatch", error);
            return false;
        }
        if (expected_date == 0U) expected_date = target_date;
        std::uint32_t source_date = 0U;
        if (!json_u32(document, "source_date", &source_date, error) ||
            source_date == 0U) {
            fail("daily static metadata JSON source_date is invalid", error);
            return false;
        }
        std::string provider;
        if (!json_string(document, "provider", &provider, error) || provider.empty()) {
            fail("daily static metadata JSON provider is missing", error);
            return false;
        }

        const nlohmann::json* source = 0;
        if (!json_member(document, "source", &source, error) ||
            !source->is_object() || !verify_json_source_hashes(*source, error))
            return false;
        const nlohmann::json* units = 0;
        if (!json_member(document, "field_units", &units, error) || !units->is_object()) {
            fail("daily static metadata JSON field_units is missing", error);
            return false;
        }
        const nlohmann::json* formulas = 0;
        if (!json_member(document, "formulas", &formulas, error) || !formulas->is_object()) {
            fail("daily static metadata JSON formulas is missing", error);
            return false;
        }
        std::uint32_t record_count = 0U;
        if (!json_u32(document, "record_count", &record_count, error)) return false;
        const nlohmann::json* records = 0;
        if (!json_member(document, "records", &records, error) || !records->is_array() ||
            records->empty() || records->size() != record_count) {
            fail("daily static metadata JSON record_count does not match records", error);
            return false;
        }

        std::size_t row_no = 0U;
        for (nlohmann::json::const_iterator item = records->begin();
             item != records->end(); ++item, ++row_no) {
            if (!item->is_object()) {
                fail("daily static metadata JSON record is not an object", error);
                return false;
            }
            std::string security;
            if (!json_string(*item, "code", &security, error) ||
                !normalize_sse_security_id(security, &security)) {
                fail("invalid or non-SSE security in daily static metadata JSON record " +
                         std::to_string(row_no), error);
                return false;
            }
            if (output->find(security) != output->end()) {
                fail("duplicate daily static metadata JSON security: " + security, error);
                return false;
            }
            DailyStaticMetadata metadata;
            if (!json_u32(*item, "date", &metadata.date, error) ||
                metadata.date != expected_date) {
                fail("daily static metadata JSON date mismatch in record " +
                         std::to_string(row_no), error);
                return false;
            }
            metadata.has_date = true;
            if (!json_record_number(*item, "free_share", &metadata.free_share, error) ||
                !json_record_number(*item, "avg_amount", &metadata.avg_amount, error) ||
                !json_record_number(*item, "prev_turnover", &metadata.prev_turnover, error) ||
                !json_record_number(*item, "turnover_threshold",
                                    &metadata.turnover_threshold, error) ||
                !json_record_number(*item, "pre_close", &metadata.pre_close, error) ||
                !json_record_number(*item, "limit_price", &metadata.limit_price, error) ||
                !json_record_number(*item, "stop_price", &metadata.stop_price, error))
                return false;
            metadata.has_free_share = true;
            metadata.has_avg_amount = true;
            metadata.has_prev_turnover = true;
            metadata.has_turnover_threshold = true;
            metadata.has_pre_close = true;
            metadata.has_limit_price = true;
            metadata.has_stop_price = true;
            if (metadata.limit_price < metadata.stop_price) {
                fail("daily static metadata JSON price bounds are inverted for " + security,
                     error);
                return false;
            }
            nlohmann::json::const_iterator optional = item->find("history_volatility_20d");
            if (optional != item->end() &&
                (!optional->is_number() || !std::isfinite(optional->get<double>()))) {
                fail("daily static metadata JSON history_volatility_20d is invalid", error);
                return false;
            }
            optional = item->find("static_position");
            if (optional != item->end() && !optional->is_number_integer()) {
                fail("daily static metadata JSON static_position is not an integer", error);
                return false;
            }
            metadata.source = provider;
            (*output)[security] = metadata;
        }
        if (output->empty() || output->size() != record_count) {
            fail("daily static metadata JSON contains no complete records", error);
            return false;
        }
        (void)source_date;
        return true;
    } catch (const std::exception& exception) {
        fail(std::string("cannot parse daily static metadata JSON: ") + exception.what(), error);
        output->clear();
        return false;
    }
}

bool load_daily_static_metadata(const std::string& path,
                                std::uint32_t expected_date,
                                DailyStaticMetadataMap* output,
                                std::string* error) {
    const std::string clean = lower(trim(path));
    if (clean.size() >= 6U && clean.compare(clean.size() - 6U, 6U, ".arrow") == 0) {
        fail("Arrow stock_day input is not parsed by the C++11 replay; run "
             "deploy/stock_day_arrow_to_csv.py --stock-day-arrow " + path +
             " --output-csv <metadata.csv>", error);
        return false;
    }
    if (clean.size() >= 5U && clean.compare(clean.size() - 5U, 5U, ".json") == 0)
        return load_daily_static_metadata_json(path, expected_date, output, error);
    return load_daily_static_metadata_csv(path, expected_date, output, error);
}

}  // namespace sse_tick
