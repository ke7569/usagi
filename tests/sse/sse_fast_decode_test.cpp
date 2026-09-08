#include "sse/market_data/sse_primary_decoder.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
bool track_allocations = false;
std::size_t allocation_count = 0;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void put32(unsigned char* p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(value >> (8 * i));
}
void put64(unsigned char* p, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(value >> (8 * i));
}
std::vector<unsigned char> packet(std::size_t count) {
    std::vector<unsigned char> bytes(count * 72, 0);
    for (std::size_t i = 0; i < count; ++i) {
        unsigned char* p = &bytes[i * 72];
        put32(p, static_cast<std::uint32_t>(i + 1)); p[8] = 0x3e;
        put64(p + 9, 0x1234567800000000ULL + i); p[17] = 1;
        std::memcpy(p + 21, "600000", 6); put32(p + 30, 9312399);
        p[34] = "ADTS"[i % 4]; put64(p + 35, 0x9876543210ULL + i);
        put64(p + 43, 0x1111222233334444ULL - i);
        put32(p + 51, 10230); put64(p + 55, 12000000 + i);
        put64(p + 63, 91827364554637281ULL); p[71] = i % 2;
    }
    return bytes;
}
bool equal(const sse_live::TickEvent& a, const sse_live::DecodedTick& b) {
    return a.security_id == b.security_id && a.channel_no == b.channel_no &&
        a.provider_sequence == b.provider_sequence && a.tick_index == b.tick_index &&
        a.app_seq_num == b.app_seq_num && a.time_of_day_micros == b.time_of_day_micros &&
        a.event_type == b.event_type && a.buy_order_no == b.buy_order_no &&
        a.sell_order_no == b.sell_order_no && a.price_raw == b.price_raw &&
        a.quantity_raw == b.quantity_raw && a.amount_raw == b.amount_raw && a.side == b.side;
}
void compare(const unsigned char* p, std::size_t size, bool equities) {
    sse_live::TickEvent old;
    sse_live::DecodedTick decoded;
    std::string error;
    sse_live::TickDecodeError status = sse_live::kTickDecodeCapacity;
    const bool expected = sse_live::decode_primary_tick(p, size, &old, &error, equities);
    const bool actual = sse_live::decode_primary_tick_fast(p, size, &decoded, &status, equities);
    check(expected == actual, "fast/legacy acceptance mismatch");
    check(error == sse_live::tick_decode_error_text(status), "fast/legacy error mismatch");
    if (actual) check(equal(old, decoded), "fast/legacy field mismatch");
}
void boundaries_and_parity() {
    std::vector<unsigned char> bytes = packet(1);
    for (unsigned position = 0; position < 72; ++position) {
        const unsigned char saved = bytes[position];
        for (unsigned value = 0; value < 256; ++value) {
            bytes[position] = static_cast<unsigned char>(value);
            compare(bytes.data(), bytes.size(), false);
            compare(bytes.data(), bytes.size(), true);
        }
        bytes[position] = saved;
    }
    const unsigned seconds[] = {0, 59, 60, 99};
    for (unsigned hhmm = 0; hhmm <= 2400; ++hhmm)
        for (unsigned s : seconds) for (unsigned cc : {0U, 99U}) {
            put32(bytes.data() + 30, hhmm * 10000 + s * 100 + cc);
            compare(bytes.data(), bytes.size(), false);
        }
    bytes = packet(1);
    std::uint32_t rng = 102939;
    for (unsigned sample = 0; sample < 10000; ++sample) {
        for (unsigned i = 0; i < 8; ++i) {
            rng = rng * 1664525U + 1013904223U;
            bytes[21 + i] = static_cast<unsigned char>(rng >> 24);
        }
        compare(bytes.data(), bytes.size(), false);
    }
    const char layouts[][9] = {"6 00000 ", " 600000 ", "600000  ", "600000\0X", "900000  "};
    for (const auto& symbol : layouts) {
        std::memcpy(bytes.data() + 21, symbol, 8);
        compare(bytes.data(), bytes.size(), true); compare(bytes.data(), bytes.size(), false);
    }
    compare(0, 72, false); compare(bytes.data(), 71, false); compare(bytes.data(), 73, false);
}
void whole_packet_and_capacity() {
    auto bytes = packet(20);
    sse_live::DecodedTick rows[20]; std::size_t count = 999, bad = 999;
    sse_live::TickDecodeError status = sse_live::kTickDecodeAbi;
    check(sse_live::decode_primary_tick_packet(bytes.data(), bytes.size(), rows, 20,
          &count, &status, &bad) && count == 20 && status == sse_live::kTickDecodeOk, "packet decode");
    for (std::size_t i = 0; i < count; ++i) {
        sse_live::TickEvent old, owned;
        check(sse_live::decode_primary_tick(bytes.data() + i * 72, 72, &old), "legacy valid packet");
        check(equal(old, rows[i]), "packet field parity");
        sse_live::materialize_tick(rows[i], &owned);
        check(equal(owned, rows[i]), "materialized field parity");
    }
    check(!sse_live::decode_primary_tick_packet(bytes.data(), bytes.size(), rows, 19,
          &count, &status) && count == 0 && status == sse_live::kTickDecodeCapacity, "capacity guard");
    bytes[19 * 72 + 8] = 0xff;
    check(!sse_live::decode_primary_tick_packet(bytes.data(), bytes.size(), rows, 20,
          &count, &status, &bad) && count == 0 && bad == 19 * 72, "partial packet must not publish count");
    check(!sse_live::decode_primary_tick_packet(bytes.data(), 1439, rows, 20,
          &count, &status) && count == 0, "truncated packet guard");
    check(!sse_live::decode_primary_tick_packet(bytes.data(), 0, rows, 20,
          &count, &status) && count == 0, "empty packet guard");
    check(!sse_live::decode_primary_tick_packet(bytes.data(), 72, rows, 20, 0), "null count guard");
}
void no_allocations() {
    const auto bytes = packet(20);
    sse_live::DecodedTick rows[20]; std::size_t count = 0;
    allocation_count = 0; track_allocations = true;
    bool ok = true;
    for (unsigned i = 0; i < 1000; ++i)
        ok = sse_live::decode_primary_tick_packet(bytes.data(), bytes.size(), rows, 20, &count) && ok;
    track_allocations = false;
    check(ok && count == 20 && allocation_count == 0, "packet hot path allocated");
}
void heartbeat() {
    unsigned char bytes[32] = {};
    put32(bytes, 999); bytes[8] = 0xa2; std::memcpy(bytes + 16, bytes, 16);
    check(sse_live::is_primary_heartbeat(bytes, 32), "known primary heartbeat rejected");
    bytes[31] = 1; check(!sse_live::is_primary_heartbeat(bytes, 32), "mismatched heartbeat accepted");
    bytes[31] = 0; bytes[5] = bytes[21] = 1;
    check(!sse_live::is_primary_heartbeat(bytes, 32), "unknown reserved metadata accepted as heartbeat");
    check(!sse_live::is_primary_heartbeat(bytes, 16), "truncated heartbeat accepted");
    check(!sse_live::is_primary_heartbeat(0, 32), "null heartbeat accepted");
}
}

void* operator new(std::size_t size) {
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    if (track_allocations) ++allocation_count;
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }

int main() {
    static_assert(std::is_pod<sse_live::DecodedTick>::value, "decoded tick must be POD");
    try {
        boundaries_and_parity(); whole_packet_and_capacity(); no_allocations(); heartbeat();
        std::cout << "sse_fast_decode_test: PASS\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
