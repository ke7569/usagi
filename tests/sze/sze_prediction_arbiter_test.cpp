#include "sze/runtime/sze_prediction_arbiter.h"

#include <iostream>

namespace {

sze_prediction::Candidate candidate(sze_prediction::Source source,
                                    std::uint64_t time_us,
                                    double turnover,
                                    std::uint64_t sequence,
                                    std::uint32_t day = 20260812U) {
    sze_prediction::Candidate value;
    value.source = source;
    value.trading_day = day;
    value.exchange_time_us = time_us;
    value.turnover = turnover;
    value.sequence = sequence;
    value.valid = true;
    return value;
}

bool expect_source(const sze_prediction::Selection& selection,
                   sze_prediction::Source source,
                   const char* label) {
    if (!selection.ready || selection.source != source) {
        std::cerr << label << " selected unexpected source" << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    sze_prediction::Arbiter arbiter;
    if (!arbiter.update(candidate(
            sze_prediction::kSnapshot, 36000000000ULL, 1000.0, 1U)) ||
        !expect_source(arbiter.select(false), sze_prediction::kSnapshot,
                       "snapshot-only")) {
        return 1;
    }

    if (!arbiter.update(candidate(
            sze_prediction::kFullOrderBook, 36000500000ULL, 1000.0, 1U)) ||
        !expect_source(arbiter.select(true), sze_prediction::kFullOrderBook,
                       "equal-turnover")) {
        return 2;
    }

    if (!arbiter.update(candidate(
            sze_prediction::kSnapshot, 36001000000ULL, 1001.0, 2U)) ||
        !expect_source(arbiter.select(true), sze_prediction::kSnapshot,
                       "snapshot-ahead")) {
        return 3;
    }

    if (!arbiter.update(candidate(
            sze_prediction::kFullOrderBook, 36001500000ULL, 1002.0, 2U)) ||
        !expect_source(arbiter.select(true), sze_prediction::kFullOrderBook,
                       "full-ahead")) {
        return 4;
    }

    if (!expect_source(arbiter.select(false), sze_prediction::kSnapshot,
                       "full-invalid")) {
        return 5;
    }

    if (!arbiter.update(candidate(
            sze_prediction::kSnapshot, 36007000000ULL, 1003.0, 3U)) ||
        !expect_source(arbiter.select(true), sze_prediction::kSnapshot,
                       "stale-full")) {
        return 6;
    }

    const sze_prediction::Selection selected = arbiter.select(true);
    if (!arbiter.should_dispatch(selected)) {
        return 7;
    }
    arbiter.mark_dispatched(selected);
    if (arbiter.should_dispatch(selected)) {
        return 8;
    }

    if (!arbiter.update(candidate(
            sze_prediction::kFullOrderBook, 36000000000ULL, 10.0, 1U,
            20260813U)) ||
        !expect_source(arbiter.select(true), sze_prediction::kFullOrderBook,
                       "next-day")) {
        return 9;
    }
    if (arbiter.update(candidate(
            sze_prediction::kSnapshot, 36000000000ULL, 20.0, 4U,
            20260812U))) {
        return 10;
    }
    return 0;
}
