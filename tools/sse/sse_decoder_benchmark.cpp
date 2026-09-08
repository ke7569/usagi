// Offline real-packet benchmark of the committed production decoder.
#include "sse/market_data/sse_primary_decoder.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <numeric>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>
#include <x86intrin.h>
#include <unistd.h>
#include <cstdlib>
#include <new>

typedef std::uint64_t U64;
volatile U64 sink = 0;
U64 now() {
    timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t)) throw std::runtime_error("clock");
    return U64(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
template<class T> T read(const std::vector<unsigned char>& b, std::size_t p) {
    if (p > b.size() || sizeof(T) > b.size() - p) throw std::runtime_error("truncated journal");
    T v; std::memcpy(&v, &b[p], sizeof(v)); return v;
}
struct Packet { const unsigned char* data; std::size_t size; U64 hw; };
U64 decode(const Packet& p) {
    U64 sum = 0;
    for (std::size_t offset = 0; offset < p.size; offset += 72) {
        sse_live::TickEvent tick;
        std::string error;
        if (!sse_live::decode_primary_tick(p.data + offset, 72, &tick, &error, false))
            throw std::runtime_error(error);
        const sse_live::TickEvent& value = tick;
        sum += value.tick_index + value.quantity_raw + value.channel_no + value.security_id[0];
    }
    return sum;
}
void stats(std::vector<U64> values) {
    if (values.empty()) { std::cout << "null"; return; }
    std::sort(values.begin(), values.end());
    const char* names[] = {"min_ns", "p05_ns", "p50_ns", "p90_ns", "p95_ns", "p99_ns", "max_ns"};
    const double qs[] = {0, .05, .5, .9, .95, .99, 1};
    std::cout << "{\"observations\":" << values.size();
    for (unsigned i = 0; i < 7; ++i) {
        const double at = qs[i] * (values.size() - 1);
        const std::size_t lo = std::size_t(at), hi = std::min(lo + 1, values.size() - 1);
        std::cout << ",\"" << names[i] << "\":" << values[lo] + (values[hi] - values[lo]) * (at - lo);
    }
    std::cout << ",\"mean_ns\":" << std::accumulate(values.begin(), values.end(), 0.0) / values.size() << '}';
}

bool tracking = false;
U64 allocations = 0;
void* operator new(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    if (tracking) ++allocations;
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }

sse_live::DecodedTick decoded[114];
__attribute__((noinline)) U64 fast(const Packet& p) {
    std::size_t count = 0;
    if (!sse_live::decode_primary_tick_packet(p.data, p.size, decoded, 114, &count))
        throw std::runtime_error("formal packet decode");
    U64 sum = 0;
    for (std::size_t i = 0; i < count; ++i)
        sum += decoded[i].tick_index + decoded[i].quantity_raw + decoded[i].channel_no + decoded[i].security_id[0];
    return sum;
}
bool equal(const sse_live::TickEvent& a, const sse_live::DecodedTick& b) {
    return a.security_id == b.security_id && a.channel_no == b.channel_no &&
        a.provider_sequence == b.provider_sequence && a.tick_index == b.tick_index &&
        a.app_seq_num == b.app_seq_num && a.time_of_day_micros == b.time_of_day_micros &&
        a.event_type == b.event_type && a.buy_order_no == b.buy_order_no &&
        a.sell_order_no == b.sell_order_no && a.price_raw == b.price_raw &&
        a.quantity_raw == b.quantity_raw && a.amount_raw == b.amount_raw && a.side == b.side;
}
U64 tsc_begin() { _mm_lfence(); U64 t = __rdtsc(); _mm_lfence(); return t; }
U64 tsc_end() { unsigned aux; U64 t = __rdtscp(&aux); _mm_lfence(); return t; }
int main(int argc, char** argv) { try {
    if (argc != 3) throw std::runtime_error("usage JOURNAL CPU");
    const int cpu = std::stoi(argv[2]);
    if (cpu < 0 || cpu >= CPU_SETSIZE) throw std::runtime_error("CPU range");
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set)) throw std::runtime_error("affinity");
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) throw std::runtime_error("open journal");
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() < 4096 || std::memcmp(&bytes[0], "SZEJRNL1", 8)) throw std::runtime_error("journal format");
    const U64 published = read<U64>(bytes, 80);
    if (published > bytes.size()) throw std::runtime_error("published size");
    std::vector<Packet> packets;
    U64 records = 0;
    for (std::size_t o = 4096; o < published;) {
        const std::size_t total = read<std::uint32_t>(bytes, o + 8), p = o + 72;
        if (total < 88 || o + total > published) throw std::runtime_error("record size");
        if (read<std::uint16_t>(bytes, p + 6) == 1) {
            const std::size_t size = read<std::uint32_t>(bytes, p + 44);
            if (p + 80 + size + 16 > o + total) throw std::runtime_error("payload size");
            if (size && size % 72 == 0) {
                packets.push_back(Packet{&bytes[p + 80], size, read<U64>(bytes, p + 48)});
                records += size / 72;
            }
        }
        o += total;
    }
    if (packets.empty()) throw std::runtime_error("empty input");
    for (const Packet& p : packets) {
        if (fast(p) != decode(p)) throw std::runtime_error("checksum mismatch");
        for (std::size_t i = 0; i < p.size / 72; ++i) {
            sse_live::TickEvent previous;
            if (!sse_live::decode_primary_tick(p.data + i * 72, 72, &previous, 0, false) ||
                !equal(previous, decoded[i])) throw std::runtime_error("field parity");
        }
    }
    allocations = 0; tracking = true;
    for (const Packet& p : packets) sink ^= fast(p);
    tracking = false;
    const U64 w0 = now(), t0 = tsc_begin(); usleep(50000);
    const U64 t1 = tsc_end(), w1 = now();
    const double rate = double(t1 - t0) / (w1 - w0);
    std::cout << std::fixed << std::setprecision(3)
        << "{\"packets\":" << packets.size() << ",\"all_fields_parity_records\":" << records
        << ",\"formal_decoder_allocations\":" << allocations << ",\"cpu\":" << sched_getcpu()
        << ",\"tsc_cycles_per_ns\":" << rate;
    for (unsigned mode = 0; mode < 2; ++mode) {
        U64 sum = 0;
        for (unsigned warm = 0; warm < 2; ++warm)
            for (const Packet& p : packets) sum += mode ? fast(p) : decode(p);
        sink ^= sum;
        std::vector<U64> all, full, sweep;
        all.reserve(packets.size() * 5); full.reserve(packets.size() * 5);
        for (unsigned pass = 0; pass < 5; ++pass) {
            sum = 0;
            for (const Packet& p : packets) {
                const U64 a = tsc_begin(); sum += mode ? fast(p) : decode(p); const U64 b = tsc_end();
                const U64 ns = U64((b - a) / rate); all.push_back(ns);
                if (p.size == 1440) full.push_back(ns);
            }
            sink ^= sum;
            sum = 0; const U64 a = now();
            for (const Packet& p : packets) sum += mode ? fast(p) : decode(p);
            sweep.push_back(now() - a); sink ^= sum;
        }
        std::cout << (mode ? ",\"formal\":{" : ",\"legacy\":{") << "\"all_packets\":";
        stats(all); std::cout << ",\"full_1440_byte_packets\":"; stats(full);
        const double mean = std::accumulate(sweep.begin(), sweep.end(), 0.) / sweep.size();
        std::cout << ",\"sweep_ns_per_record\":" << mean / records
                  << ",\"sweep_ns_per_packet\":" << mean / packets.size() << '}';
    }
    std::cout << ",\"checksum\":" << sink << "}\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
