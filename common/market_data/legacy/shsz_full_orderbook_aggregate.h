#ifndef SHSZ_FULL_ORDERBOOK_AGGREGATE_H
#define SHSZ_FULL_ORDERBOOK_AGGREGATE_H

#include <cstdint>

struct ShSzFullOrderSum {
    double price;
    double volume_sum;
    int64_t count_sum;
    int64_t tsc_sum;
    double amt_sum;
    double window_volume_sum;

    ShSzFullOrderSum()
        : price(0.0),
          volume_sum(0.0),
          count_sum(0),
          tsc_sum(0),
          amt_sum(0.0),
          window_volume_sum(0.0) {
    }
};

struct ShSzFullOb {
    ShSzFullOrderSum ask_01;
    ShSzFullOrderSum bid_01;
    ShSzFullOrderSum ask_05;
    ShSzFullOrderSum bid_05;
    ShSzFullOrderSum ask_10;
    ShSzFullOrderSum bid_10;
    ShSzFullOrderSum ask_level1;
    ShSzFullOrderSum bid_level1;
    ShSzFullOrderSum ask_level5;
    ShSzFullOrderSum bid_level5;

    int64_t ask_max_volume;
    int64_t bid_max_volume;
    double ask_max_level_price;
    double bid_max_level_price;
    int64_t ask_total_count;
    int64_t bid_total_count;
    double mp;
    bool valid;

    ShSzFullOb()
        : ask_01(),
          bid_01(),
          ask_05(),
          bid_05(),
          ask_10(),
          bid_10(),
          ask_level1(),
          bid_level1(),
          ask_level5(),
          bid_level5(),
          ask_max_volume(0),
          bid_max_volume(0),
          ask_max_level_price(0.0),
          bid_max_level_price(0.0),
          ask_total_count(0),
          bid_total_count(0),
          mp(0.0),
          valid(false) {
    }
};

#endif // SHSZ_FULL_ORDERBOOK_AGGREGATE_H
