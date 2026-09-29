#ifndef SSE_T0_TICK_FACTORS_H
#define SSE_T0_TICK_FACTORS_H

#include "sse/market_data/sse_tick_order_book.h"
#include "sse/market_data/sse_tick_static_metadata.h"

#include <array>
#include <cstdint>
#include <vector>
#include <memory>

namespace sse_tick {

static const std::size_t kTickFactorCount = 50U;

struct FactorValidity {
    bool has_two_sided_book;
    bool has_previous_book;
    bool has_flow;
    bool has_free_share;
    bool has_static_metadata;
    bool static_metadata_complete;
    bool complete;
    FactorValidity();
};

struct FactorRow {
    std::array<float, kTickFactorCount> values;
    FactorValidity validity;
    double mid_price;
    std::uint64_t tick_index;
    FactorRow();
};

const char* tick_factor_name(std::size_t index);

class FactorState {
public:
    FactorState();
    void prepare_book(OrderBook&,std::uint64_t);
    void set_v06(bool enabled) { v06_ = enabled; }
    void reset();
    void set_free_share(double value);
    void set_static_metadata(const DailyStaticMetadata& metadata);
    // Seeds the first CompleteOrderBookSH sampling window when its opening
    // Level2 cut has no two-sided depth and therefore falls back to pre-close.
    void seed_window(double mid_price, std::uint64_t now_micros,
                     double volume = 0.0, double turnover = 0.0);
    bool has_free_share() const { return have_free_share_; }
    bool has_static_metadata() const { return have_static_metadata_; }
    double turnover_threshold() const {
        return have_static_metadata_ && static_metadata_.has_turnover_threshold
            ? static_metadata_.turnover_threshold : 0.0;
    }
    double pre_close_price() const {
        return have_static_metadata_ && static_metadata_.has_pre_close
            ? static_metadata_.pre_close : 0.0;
    }
    bool static_metadata_complete() const {
        return have_static_metadata_ && static_metadata_.complete();
    }
    const char* static_quality() const;
    // This is intentionally non-const: a sample consumes the event window
    // accumulated since the preceding Level2 cut.
    FactorRow build(OrderBook& book, std::uint64_t now_micros,
                    double snapshot_last_price = 0.0,
                    double snapshot_volume = -1.0,
                    double snapshot_turnover = -1.0);
    // Use the same immutable ten-level cut already taken by the sample gate.
    FactorRow build(OrderBook& book, std::uint64_t now_micros,
                    const Level (&bids)[10], const Level (&asks)[10],
                    double snapshot_last_price = 0.0,
                    double snapshot_volume = -1.0,
                    double snapshot_turnover = -1.0);

private:
    struct Prepared;std::shared_ptr<Prepared> prepared_;
    bool v06_;
    struct BookPoint {
        double mid;
        double spread;
        double bid_qty;
        double ask_qty;
        double volume;
        double turnover;
        std::uint64_t tick;
        std::uint64_t time_micros;
        Level bids[10];
        Level asks[10];
        bool two_sided;
    };
    bool have_previous_;
    BookPoint previous_;
    double free_share_;
    bool have_free_share_;
    DailyStaticMetadata static_metadata_;
    bool have_static_metadata_;
    // Reused between samples so consuming a flow window does not allocate a
    // fresh event vector at every batch end.
    FlowStats flow_window_;
};

}  // namespace sse_tick

#endif  // SSE_T0_TICK_FACTORS_H
