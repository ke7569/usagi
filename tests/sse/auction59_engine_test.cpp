#include "sse/auction/auction59_engine.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << "\n"; std::exit(1); }
}

std::uint64_t at(int hour, int minute, int second) {
    return static_cast<std::uint64_t>(hour * 3600 + minute * 60 + second) * 1000000ULL;
}

sse_live::TickEvent order(char type, std::uint64_t seq, std::uint64_t time,
                          bool buy, std::uint64_t id, std::uint32_t price,
                          std::uint64_t quantity) {
    sse_live::TickEvent event;
    event.security_id = "600001"; event.channel_no = 1; event.app_seq_num = seq;
    event.time_of_day_micros = time; event.event_type = type;
    event.buy_order_no = buy ? id : 0; event.sell_order_no = buy ? 0 : id;
    event.price_raw = price; event.quantity_raw = quantity * 1000ULL;
    event.amount_raw = 0; event.side = buy ? 0 : 1;
    return event;
}

sse_live::TickEvent trade(std::uint64_t seq, std::uint64_t buy, std::uint64_t sell,
                          std::uint64_t quantity) {
    sse_live::TickEvent event;
    event.security_id = "600001"; event.channel_no = 1; event.app_seq_num = seq;
    event.time_of_day_micros = at(9, 25, 0); event.event_type = 'T';
    event.buy_order_no = buy; event.sell_order_no = sell; event.price_raw = 10000;
    event.quantity_raw = quantity * 1000ULL; event.amount_raw = 0; event.side = 0;
    return event;
}

std::uint32_t bits(float value) {
    std::uint32_t output; std::memcpy(&output, &value, sizeof(output)); return output;
}

}  // namespace

int main() {
    sse_auction59::StaticMetadata metadata;
    metadata.date = 20260819; metadata.pre_close = 10.0;
    metadata.lower_limit = 9.0; metadata.upper_limit = 11.0;
    metadata.limits_valid = true;
    sse_auction59::AuctionAccumulator accumulator("600001", metadata);
    std::uint64_t arrival = 1;
    const sse_live::TickEvent events[] = {
        order('A', 1, at(9,15,0), true, 1, 10000, 80),
        order('A', 2, at(9,15,0), false, 2, 9980, 70),
        order('A', 3, at(9,17,0), true, 3, 10020, 120),
        order('A', 4, at(9,17,0), false, 4, 9990, 90),
        order('A', 5, at(9,18,0), true, 5, 10000, 50),
        order('A', 6, at(9,18,0), false, 6, 9990, 60),
        order('D', 7, at(9,18,4), true, 5, 10000, 50),
        order('D', 8, at(9,18,4), false, 6, 9990, 60),
        order('A', 9, at(9,19,0), false, 7, 10000, 80),
        order('A', 10, at(9,20,0), false, 8, 10010, 40),
        order('A', 11, at(9,21,0), true, 9, 10000, 50),
        order('A', 12, at(9,24,0), true, 10, 9990, 30),
        trade(13, 1, 2, 70), trade(14, 1, 4, 10), trade(15, 3, 4, 80),
        trade(16, 3, 7, 40), trade(17, 9, 7, 40)
    };
    for (std::size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i)
        accumulator.observe(events[i], 0, arrival++);
    sse_live::TickEvent status = order('S', 18, at(9,25,0), true, 0, 0, 0);
    status.buy_order_no = status.sell_order_no = status.quantity_raw = 0;
    status.side = 3;
    accumulator.observe(status, 0, arrival++);
    const sse_auction59::AuctionResult output = accumulator.finalize();
    require(output.hard_valid, "enumerable fixture is not hard valid");
    require(output.observed_auction_qty == 240 && output.reconstructed_auction_qty == 240,
            "opening quantity mismatch");
    const std::uint32_t expected[] = {
        0,0,0,380940392,0,2143289344U,2143289344U,0,0,0,2143289344U,0,1065353216,
        1142292480,0,1090065301,1017589509,1026206379,1062956471,1043978508,0,
        3135863621U,1017589509,1018040932,0,3161753997U,1051372203,1065353216,
        1063046926,1065353216,2143289344U,3200253952U,3212836864U,1054448026,
        1023625269,1023969417,1045220557,1045220557,1035611788,1035611788,0,0,
        3192704205U,1056964608,0,3197737370U,3209481421U,1017589509,0,0,0,0,
        1065353216,1056964608,1065353216,1056964608,1051372203,1042983595,0,
        1056964608,1052770304,3187671040U
    };
    require(output.canonical_valid_mask == 0x3fffffffBffffb9fULL,
            "canonical factor valid mask mismatch");
    for (std::size_t i = 0; i < sse_auction59::kCanonicalFactorCount; ++i) {
        if (bits(output.canonical_factors[i]) != expected[i]) {
            std::cerr << "factor bit mismatch index=" << i << " actual="
                      << bits(output.canonical_factors[i]) << " expected=" << expected[i] << "\n";
            return 1;
        }
    }
    require(output.projected_valid_mask == 0x7ffffffeffffb9fULL,
            "Auction59 projected valid mask mismatch");
    std::cout << "auction59_engine_test: ok\n";
    return 0;
}
