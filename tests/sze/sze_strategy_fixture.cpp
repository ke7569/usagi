#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char* const kFactors[] = {
    "factor_spread_permille", "factor_mid_return_permille",
    "factor_weighted_return_permille_1", "factor_weighted_return_permille_2",
    "factor_weighted_return_permille_3", "factor_weighted_return_permille_4",
    "factor_weighted_return_permille_5", "factor_weighted_volume_imbalance",
    "factor_volume_imbalance", "factor_percent_turnover",
    "factor_liquidity_ask_l1_share", "factor_liquidity_bid_l1_share",
    "factor_hermes_permille", "factor_tr_sqrt_positive", "factor_fee_on_tick",
    "factor_bid_volume_change_ratio", "factor_ask_volume_change_ratio",
    "factor_weighted_ask_permille", "factor_weighted_bid_permille",
    "factor_weighted_ask_return_permille", "factor_weighted_bid_return_permille",
    "factor_positive_fill_rate", "factor_negative_fill_rate",
    "factor_order_flow_imbalance", "factor_cfr_imbalance",
    "factor_book_count_imbalance_l1", "factor_book_count_imbalance_l5",
    "factor_book_avg_size_imbalance_l1", "factor_book_avg_size_imbalance_l5",
    "factor_book_life_imbalance_l1", "factor_book_life_imbalance_l5",
    "factor_book_fixdist_imbalance_1pct", "factor_book_fixdist_imbalance_5pct",
    "factor_book_fixdist_weighted_1pct", "factor_book_fixdist_weighted_5pct",
    "factor_book_avg_size_imbalance", "factor_book_count_imbalance",
    "factor_book_life_imbalance", "factor_book_young_imbalance_1pct",
    "factor_max_bid_distance_ratio", "factor_max_ask_distance_ratio",
    "factor_max_vol_distance_imbalance", "factor_book_fixdist_hermes",
    "factor_positive_order_flow_log1p", "factor_negative_order_flow_log1p",
    "factor_market_flow_asinh", "factor_cancel_buy_flow_log1p",
    "factor_cancel_sell_flow_log1p", "factor_positive_trade_log1p",
    "factor_negative_trade_log1p"
};

const char* const kTensors[] = {
    "input_norm.weight", "input_norm.bias", "input_proj.weight",
    "input_proj.bias", "gru.weight_ih_l0", "gru.weight_hh_l0",
    "gru.bias_ih_l0", "gru.bias_hh_l0", "gru.weight_ih_l1",
    "gru.weight_hh_l1", "gru.bias_ih_l1", "gru.bias_hh_l1",
    "head.weight", "head.bias"
};

void put_u16(std::ofstream* out, std::uint16_t value) {
    out->put(static_cast<char>(value & 0xffU));
    out->put(static_cast<char>((value >> 8U) & 0xffU));
}

void put_u32(std::ofstream* out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out->put(static_cast<char>((value >> shift) & 0xffU));
}

void put_u64(std::ofstream* out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out->put(static_cast<char>((value >> shift) & 0xffU));
}

void put_f32(std::ofstream* out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

void put_string(std::ofstream* out, const char* value) {
    const std::string text(value);
    put_u16(out, static_cast<std::uint16_t>(text.size()));
    out->write(text.data(), static_cast<std::streamsize>(text.size()));
}

void put_hash(std::ofstream* out, const char* hex) {
    for (unsigned i = 0; i < 32; ++i) {
        const char high = hex[2 * i];
        const char low = hex[2 * i + 1];
        const unsigned high_value = high <= '9' ? high - '0' : high - 'a' + 10;
        const unsigned low_value = low <= '9' ? low - '0' : low - 'a' + 10;
        out->put(static_cast<char>((high_value << 4U) | low_value));
    }
}

void tensor(std::ofstream* out, const char* name,
            const std::vector<std::uint32_t>& shape, float fill) {
    put_string(out, name);
    out->put(static_cast<char>(shape.size()));
    std::uint64_t count = 1;
    for (std::size_t i = 0; i < shape.size(); ++i) {
        put_u32(out, shape[i]);
        count *= shape[i];
    }
    put_u64(out, count * sizeof(float));
    for (std::uint64_t i = 0; i < count; ++i) put_f32(out, fill);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sze_strategy_fixture OUTPUT_MODEL\n";
        return 2;
    }
    std::ifstream existing(argv[1], std::ios::binary);
    if (existing.good()) {
        std::cerr << "refusing to overwrite existing model fixture\n";
        return 3;
    }
    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    if (!output.is_open()) return 1;
    output.write("MIX15306", 8);
    put_u32(&output, 1U);             // format version
    put_u32(&output, 0x01020304U);    // little-endian marker
    put_u32(&output, 50U);            // features
    put_u32(&output, 128U);           // hidden size
    put_u32(&output, 2U);             // layers
    put_u32(&output, 14U);            // tensors
    put_f32(&output, 1.0e-5f);
    put_u32(&output, 50U);
    put_hash(&output, "09ed1cf8b824d75708faf725cf14797c3b0f32635dbad59374c04f4ff7fb3bb5");
    put_hash(&output, "e20ed70098a025f597f8b9cda41fb79b3188d875ad227f243c872ddbfbbed97e");
    for (std::size_t i = 0; i < 50U; ++i) put_string(&output, kFactors[i]);

    tensor(&output, kTensors[0], std::vector<std::uint32_t>(1, 50U), 1.0f);
    tensor(&output, kTensors[1], std::vector<std::uint32_t>(1, 50U), 0.0f);
    tensor(&output, kTensors[2], std::vector<std::uint32_t>{128U, 50U}, 0.0f);
    tensor(&output, kTensors[3], std::vector<std::uint32_t>(1, 128U), 0.0f);
    tensor(&output, kTensors[4], std::vector<std::uint32_t>{384U, 128U}, 0.0f);
    tensor(&output, kTensors[5], std::vector<std::uint32_t>{384U, 128U}, 0.0f);
    tensor(&output, kTensors[6], std::vector<std::uint32_t>(1, 384U), 0.0f);
    tensor(&output, kTensors[7], std::vector<std::uint32_t>(1, 384U), 0.0f);
    tensor(&output, kTensors[8], std::vector<std::uint32_t>{384U, 128U}, 0.0f);
    tensor(&output, kTensors[9], std::vector<std::uint32_t>{384U, 128U}, 0.0f);
    tensor(&output, kTensors[10], std::vector<std::uint32_t>(1, 384U), 0.0f);
    tensor(&output, kTensors[11], std::vector<std::uint32_t>(1, 384U), 0.0f);
    tensor(&output, kTensors[12], std::vector<std::uint32_t>{1U, 128U}, 0.0f);
    tensor(&output, kTensors[13], std::vector<std::uint32_t>(1, 1U), 100.0f);
    return output.good() ? 0 : 1;
}
