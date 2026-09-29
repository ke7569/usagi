#include "auction59_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

namespace sse_auction59 {
namespace {

const std::uint64_t kDayMicros = 24ULL * 60ULL * 60ULL * 1000000ULL;
const std::uint64_t kAuctionStart = (9ULL * 3600ULL + 15ULL * 60ULL) * 1000000ULL;
const std::uint64_t kCancelEnd = (9ULL * 3600ULL + 20ULL * 60ULL) * 1000000ULL;
const std::uint64_t kAuctionEnd = (9ULL * 3600ULL + 25ULL * 60ULL) * 1000000ULL;
const std::uint64_t kContinuousStart = (9ULL * 3600ULL + 30ULL * 60ULL) * 1000000ULL;
const std::int32_t kQuoteTick = 10;
const std::size_t kFullEnd = 600U;
const std::size_t kLateStart = 300U;
const std::size_t kLast60Start = 540U;

std::uint64_t reason(QualityReason value) { return 1ULL << static_cast<unsigned>(value); }

float canonical_nan() {
    const std::uint32_t bits = 0x7fc00000U;
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::int32_t price_tick(double price) {
    if (!std::isfinite(price) || price <= 0.0) return 0;
    const double scaled = std::floor(price * 1000.0 + 0.5);
    if (scaled > static_cast<double>(std::numeric_limits<std::int32_t>::max())) return 0;
    return static_cast<std::int32_t>(scaled);
}

std::int32_t ratio_quote_tick(std::int32_t value, std::int64_t numerator,
                              std::int64_t denominator) {
    if (value <= 0 || numerator <= 0 || denominator <= 0) return 0;
    const std::int64_t quote_denominator = denominator * kQuoteTick;
    const std::int64_t scaled = static_cast<std::int64_t>(value) * numerator;
    return static_cast<std::int32_t>(((scaled + quote_denominator / 2) /
                                      quote_denominator) * kQuoteTick);
}

struct NewOrder {
    std::uint64_t order_id, app_seq, exchange_time_micros, arrival_index;
    Side side;
    std::int32_t price_tick;
    std::int64_t qty;
};

struct Cancel {
    std::uint64_t app_seq, exchange_time_micros, arrival_index;
    Side side;
    std::int32_t price_tick;
    std::int64_t qty, order_original_qty, lifetime_micros;
};

struct Trade {
    std::uint64_t app_seq, exchange_time_micros, realtime_ns, arrival_index;
    std::uint32_t channel_no;
    std::int32_t price_tick;
    std::int64_t qty;
    std::uint64_t buy_order_id, sell_order_id;
};

struct OrderState {
    std::uint64_t app_seq, exchange_time_micros;
    Side side;
    std::int32_t price_tick;
    std::int64_t original_qty, cancelled_qty, opening_fill_qty, residual_qty;
};

struct PriceBook {
    std::map<std::int32_t, std::int64_t> buy, sell;

    void add(Side side, std::int32_t price, std::int64_t qty) {
        std::map<std::int32_t, std::int64_t>& levels = side == kBuy ? buy : sell;
        levels[price] += qty;
    }
    bool remove(Side side, std::int32_t price, std::int64_t qty) {
        std::map<std::int32_t, std::int64_t>& levels = side == kBuy ? buy : sell;
        std::map<std::int32_t, std::int64_t>::iterator found = levels.find(price);
        if (found == levels.end() || found->second < qty) return false;
        found->second -= qty;
        if (found->second == 0) levels.erase(found);
        return true;
    }
    std::int64_t demand(std::int32_t price) const {
        std::int64_t total = 0;
        for (std::map<std::int32_t, std::int64_t>::const_iterator it = buy.lower_bound(price);
             it != buy.end(); ++it) total += it->second;
        return total;
    }
    std::int64_t supply(std::int32_t price) const {
        std::int64_t total = 0;
        for (std::map<std::int32_t, std::int64_t>::const_iterator it = sell.begin();
             it != sell.end() && it->first <= price; ++it) total += it->second;
        return total;
    }
    std::int64_t match(std::int32_t price) const {
        return std::min(demand(price), supply(price));
    }
    std::int64_t total(Side side) const {
        const std::map<std::int32_t, std::int64_t>& levels = side == kBuy ? buy : sell;
        std::int64_t value = 0;
        for (std::map<std::int32_t, std::int64_t>::const_iterator it = levels.begin();
             it != levels.end(); ++it) value += it->second;
        return value;
    }
};

struct Clearing {
    bool valid;
    std::int32_t price_tick;
    std::int64_t match_qty, demand, supply;
    Clearing() : valid(false), price_tick(0), match_qty(0), demand(0), supply(0) {}
};

struct Candidate {
    std::int32_t price;
    std::int64_t match, demand, supply;
};

Clearing clear_book(const PriceBook& book, std::int32_t lower, std::int32_t upper,
                    bool* unresolved) {
    Clearing output;
    if (unresolved) *unresolved = false;
    if (lower <= 0 || upper < lower || book.buy.empty() || book.sell.empty()) return output;
    std::set<std::int32_t> price_set;
    for (std::map<std::int32_t, std::int64_t>::const_iterator it = book.buy.begin();
         it != book.buy.end(); ++it) if (it->first >= lower && it->first <= upper) price_set.insert(it->first);
    for (std::map<std::int32_t, std::int64_t>::const_iterator it = book.sell.begin();
         it != book.sell.end(); ++it) if (it->first >= lower && it->first <= upper) price_set.insert(it->first);
    if (price_set.empty()) return output;

    std::int64_t total_buy = book.total(kBuy), buy_below = 0, sell_through = 0;
    std::map<std::int32_t, std::int64_t>::const_iterator bi = book.buy.begin();
    std::map<std::int32_t, std::int64_t>::const_iterator si = book.sell.begin();
    std::vector<Candidate> candidates;
    for (std::set<std::int32_t>::const_iterator pi = price_set.begin(); pi != price_set.end(); ++pi) {
        const std::int32_t price = *pi;
        while (bi != book.buy.end() && bi->first < price) { buy_below += bi->second; ++bi; }
        const std::int64_t buy_at = bi != book.buy.end() && bi->first == price ? bi->second : 0;
        std::int64_t sell_at = 0;
        while (si != book.sell.end() && si->first <= price) {
            sell_through += si->second;
            if (si->first == price) sell_at = si->second;
            ++si;
        }
        const std::int64_t demand = total_buy - buy_below;
        const std::int64_t supply = sell_through;
        const std::int64_t matched = std::min(demand, supply);
        if (demand - buy_at <= matched && supply - sell_at <= matched &&
            (demand == matched || supply == matched)) {
            Candidate value = {price, matched, demand, supply};
            candidates.push_back(value);
        }
    }
    std::int64_t max_match = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) max_match = std::max(max_match, candidates[i].match);
    if (max_match <= 0) return output;
    std::uint64_t min_imbalance = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i < candidates.size(); ++i) if (candidates[i].match == max_match) {
        const std::int64_t delta = candidates[i].demand - candidates[i].supply;
        const std::uint64_t absolute = static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
        min_imbalance = std::min(min_imbalance, absolute);
    }
    std::int32_t low = std::numeric_limits<std::int32_t>::max(), high = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const std::int64_t delta = candidates[i].demand - candidates[i].supply;
        const std::uint64_t absolute = static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
        if (candidates[i].match == max_match && absolute == min_imbalance) {
            low = std::min(low, candidates[i].price);
            high = std::max(high, candidates[i].price);
        }
    }
    if (high == 0) { if (unresolved) *unresolved = true; return output; }
    const std::int32_t chosen = ((low + high + kQuoteTick) / (2 * kQuoteTick)) * kQuoteTick;
    if (chosen < lower || chosen > upper) { if (unresolved) *unresolved = true; return output; }
    output.valid = true;
    output.price_tick = chosen;
    output.demand = book.demand(chosen);
    output.supply = book.supply(chosen);
    output.match_qty = std::min(output.demand, output.supply);
    if (output.match_qty != max_match) { output.valid = false; if (unresolved) *unresolved = true; }
    return output;
}

struct GridPoint {
    Clearing clearing;
    std::int64_t anchor_buy, anchor_sell;
};

struct Maybe {
    bool valid;
    double value;
    Maybe() : valid(false), value(0.0) {}
    explicit Maybe(double v) : valid(std::isfinite(v)), value(v) {}
};

Maybe divide(std::int64_t numerator, std::int64_t denominator) {
    return denominator != 0 ? Maybe(static_cast<double>(numerator) / denominator) : Maybe();
}

Maybe imbalance(std::int64_t buy, std::int64_t sell) {
    if (buy > std::numeric_limits<std::int64_t>::max() - sell) return Maybe();
    const std::int64_t total = buy + sell;
    return total > 0 ? divide(buy - sell, total) : Maybe();
}

Maybe log_return(std::int32_t price, std::int32_t base) {
    return price > 0 && base > 0 ? Maybe(10000.0 * std::log(static_cast<double>(price) / base)) : Maybe();
}

void set_factor(AuctionResult* result, std::size_t bit, const Maybe& value) {
    if (!value.valid || !std::isfinite(value.value)) return;
    const float narrowed = static_cast<float>(value.value);
    if (!std::isfinite(narrowed)) { result->quality_reason_mask |= reason(kNumericOverflow); return; }
    result->canonical_factors[bit] = narrowed;
    result->canonical_valid_mask |= 1ULL << bit;
}

std::int64_t event_second(std::uint64_t time) {
    return static_cast<std::int64_t>((time % kDayMicros - kAuctionStart) / 1000000ULL);
}

std::vector<NewOrder> order_window(const std::vector<NewOrder>& orders,
                                   std::int64_t start, std::int64_t end) {
    std::vector<NewOrder> output;
    for (std::size_t i = 0; i < orders.size(); ++i) {
        const std::int64_t second = event_second(orders[i].exchange_time_micros);
        if (second >= start && second <= end) output.push_back(orders[i]);
    }
    return output;
}

std::pair<std::int64_t, std::int64_t> side_quantities(const std::vector<NewOrder>& orders) {
    std::pair<std::int64_t, std::int64_t> output(0, 0);
    for (std::size_t i = 0; i < orders.size(); ++i)
        (orders[i].side == kBuy ? output.first : output.second) += orders[i].qty;
    return output;
}

Maybe regression_slope(const std::vector<std::pair<double, double> >& samples) {
    if (samples.size() < 2U) return Maybe();
    double mx = 0.0, my = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i) { mx += samples[i].first; my += samples[i].second; }
    mx /= samples.size(); my /= samples.size();
    double variance = 0.0, covariance = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        variance += (samples[i].first - mx) * (samples[i].first - mx);
        covariance += (samples[i].first - mx) * (samples[i].second - my);
    }
    return variance != 0.0 ? Maybe(covariance / variance) : Maybe();
}

bool path_window(const std::vector<GridPoint>& grid, std::size_t start, std::size_t end,
                 std::vector<std::pair<std::size_t, Clearing> >* path) {
    path->clear();
    for (std::size_t i = start; i <= end && i < grid.size(); ++i)
        if (grid[i].clearing.valid) path->push_back(std::make_pair(i, grid[i].clearing));
    const std::size_t total = end - start + 1U;
    return path->size() * 10U >= total * 8U && !path->empty() && path->back().first == end;
}

Maybe path_return(const std::vector<std::pair<std::size_t, Clearing> >& path) {
    return path.empty() ? Maybe() : log_return(path.back().second.price_tick, path.front().second.price_tick);
}

Maybe path_slope(const std::vector<std::pair<std::size_t, Clearing> >& path) {
    std::vector<std::pair<double, double> > samples;
    for (std::size_t i = 0; i < path.size(); ++i)
        samples.push_back(std::make_pair(path[i].first / 60.0, std::log(static_cast<double>(path[i].second.price_tick))));
    Maybe value = regression_slope(samples);
    if (value.valid) value.value *= 10000.0;
    return value;
}

Maybe path_r2(const std::vector<std::pair<std::size_t, Clearing> >& path) {
    if (path.size() < 2U) return Maybe();
    bool constant = true;
    for (std::size_t i = 1; i < path.size(); ++i)
        if (path[i].second.price_tick != path[0].second.price_tick) constant = false;
    if (constant) return Maybe();
    std::vector<std::pair<double, double> > samples;
    for (std::size_t i = 0; i < path.size(); ++i)
        samples.push_back(std::make_pair(path[i].first / 60.0, std::log(static_cast<double>(path[i].second.price_tick))));
    Maybe slope = regression_slope(samples);
    if (!slope.valid) return Maybe();
    double mx = 0.0, my = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i) { mx += samples[i].first; my += samples[i].second; }
    mx /= samples.size(); my /= samples.size();
    const double intercept = my - slope.value * mx;
    double total = 0.0, residual = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        total += (samples[i].second - my) * (samples[i].second - my);
        const double error = samples[i].second - (intercept + slope.value * samples[i].first);
        residual += error * error;
    }
    return total != 0.0 ? Maybe(1.0 - residual / total) : Maybe();
}

Maybe path_efficiency(const std::vector<std::pair<std::size_t, Clearing> >& path) {
    if (path.empty()) return Maybe();
    double variation = 0.0;
    for (std::size_t i = 1; i < path.size(); ++i)
        variation += std::fabs(std::log(static_cast<double>(path[i].second.price_tick) /
                                        path[i - 1U].second.price_tick));
    return variation != 0.0 ? Maybe(std::fabs(std::log(static_cast<double>(path.back().second.price_tick) /
                                                       path.front().second.price_tick)) / variation) : Maybe();
}

Maybe path_range(const std::vector<std::pair<std::size_t, Clearing> >& path) {
    if (path.empty()) return Maybe();
    std::int32_t low = path[0].second.price_tick, high = low;
    for (std::size_t i = 1; i < path.size(); ++i) {
        low = std::min(low, path[i].second.price_tick); high = std::max(high, path[i].second.price_tick);
    }
    return Maybe(10000.0 * std::log(static_cast<double>(high) / low));
}

Maybe path_drawdown(const std::vector<std::pair<std::size_t, Clearing> >& path) {
    if (path.empty()) return Maybe();
    double peak = -std::numeric_limits<double>::infinity(), drawdown = 0.0;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const double value = std::log(static_cast<double>(path[i].second.price_tick));
        peak = std::max(peak, value); drawdown = std::max(drawdown, peak - value);
    }
    return Maybe(10000.0 * drawdown);
}

Maybe path_end_position(const std::vector<std::pair<std::size_t, Clearing> >& path) {
    if (path.empty()) return Maybe();
    std::int32_t low = path[0].second.price_tick, high = low;
    for (std::size_t i = 1; i < path.size(); ++i) {
        low = std::min(low, path[i].second.price_tick); high = std::max(high, path[i].second.price_tick);
    }
    return high != low ? Maybe(static_cast<double>(path.back().second.price_tick - low) / (high - low)) : Maybe();
}

std::vector<GridPoint> build_grid(const PriceBook& initial_empty, std::int32_t lower,
                                  std::int32_t upper, std::int32_t opening,
                                  const std::vector<NewOrder>& orders,
                                  const std::vector<Cancel>& cancels) {
    (void)initial_empty;
    struct Event { std::uint64_t time, seq, arrival; bool add; std::size_t index; };
    std::vector<Event> events;
    for (std::size_t i = 0; i < orders.size(); ++i) {
        Event e = {orders[i].exchange_time_micros, orders[i].app_seq, orders[i].arrival_index, true, i}; events.push_back(e);
    }
    for (std::size_t i = 0; i < cancels.size(); ++i) {
        Event e = {cancels[i].exchange_time_micros, cancels[i].app_seq, cancels[i].arrival_index, false, i}; events.push_back(e);
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.time != b.time) return a.time < b.time;
        if (a.arrival != b.arrival) return a.arrival < b.arrival;
        if (a.seq != b.seq) return a.seq < b.seq;
        return a.add && !b.add;
    });
    PriceBook book;
    std::vector<GridPoint> grid;
    grid.reserve(601U);
    std::size_t cursor = 0U;
    GridPoint previous;
    previous.anchor_buy = previous.anchor_sell = 0;
    for (std::size_t second = 0; second <= 600U; ++second) {
        const std::uint64_t time = kAuctionStart + second * 1000000ULL;
        bool changed = second == 0U;
        while (cursor < events.size() && events[cursor].time % kDayMicros <= time) {
            if (events[cursor].add) {
                const NewOrder& e = orders[events[cursor].index]; book.add(e.side, e.price_tick, e.qty);
            } else {
                const Cancel& e = cancels[events[cursor].index]; book.remove(e.side, e.price_tick, e.qty);
            }
            ++cursor; changed = true;
        }
        GridPoint point = previous;
        if (changed) {
            bool unresolved = false;
            point.clearing = clear_book(book, lower, upper, &unresolved);
            point.anchor_buy = book.demand(opening); point.anchor_sell = book.supply(opening);
        }
        grid.push_back(point); previous = point;
    }
    return grid;
}

std::pair<std::int64_t, std::int64_t> anchor_before(std::int32_t opening,
                                                    const std::vector<NewOrder>& orders,
                                                    const std::vector<Cancel>& cancels) {
    struct Event { std::uint64_t time, arrival; bool add; std::size_t index; };
    std::vector<Event> events;
    for (std::size_t i = 0; i < orders.size(); ++i) if (orders[i].exchange_time_micros % kDayMicros < kCancelEnd) {
        Event e = {orders[i].exchange_time_micros, orders[i].arrival_index, true, i}; events.push_back(e);
    }
    for (std::size_t i = 0; i < cancels.size(); ++i) if (cancels[i].exchange_time_micros % kDayMicros < kCancelEnd) {
        Event e = {cancels[i].exchange_time_micros, cancels[i].arrival_index, false, i}; events.push_back(e);
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.time != b.time ? a.time < b.time : a.arrival < b.arrival;
    });
    PriceBook book;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].add) { const NewOrder& e = orders[events[i].index]; book.add(e.side, e.price_tick, e.qty); }
        else { const Cancel& e = cancels[events[i].index]; book.remove(e.side, e.price_tick, e.qty); }
    }
    return std::make_pair(book.demand(opening), book.supply(opening));
}

Maybe order_imbalance(const std::vector<NewOrder>& orders) {
    const std::pair<std::int64_t, std::int64_t> values = side_quantities(orders);
    return imbalance(values.first, values.second);
}

double aggression(const NewOrder& order, std::int32_t opening) {
    const double sign = order.side == kBuy ? 1.0 : -1.0;
    return sign * (order.price_tick - opening) / static_cast<double>(kQuoteTick);
}

Maybe marketable_imbalance(const std::vector<NewOrder>& orders, std::int32_t opening) {
    std::int64_t buy = 0, sell = 0;
    for (std::size_t i = 0; i < orders.size(); ++i) if (aggression(orders[i], opening) >= 0.0)
        (orders[i].side == kBuy ? buy : sell) += orders[i].qty;
    return imbalance(buy, sell);
}

Maybe aggression_mean(const std::vector<NewOrder>& orders, Side side, std::int32_t opening) {
    std::int64_t qty = 0; double weighted = 0.0;
    for (std::size_t i = 0; i < orders.size(); ++i) if (orders[i].side == side) {
        qty += orders[i].qty; weighted += orders[i].qty * aggression(orders[i], opening);
    }
    return qty > 0 ? Maybe(weighted / qty) : Maybe();
}

Maybe arrival_gap(const std::vector<NewOrder>& orders) {
    std::int64_t buy_qty = 0, sell_qty = 0; double buy = 0.0, sell = 0.0;
    for (std::size_t i = 0; i < orders.size(); ++i) {
        const double time = (event_second(orders[i].exchange_time_micros) - 300) / 300.0;
        if (orders[i].side == kBuy) { buy_qty += orders[i].qty; buy += orders[i].qty * time; }
        else { sell_qty += orders[i].qty; sell += orders[i].qty * time; }
    }
    return buy_qty > 0 && sell_qty > 0 ? Maybe(buy / buy_qty - sell / sell_qty) : Maybe();
}

std::vector<std::pair<std::int64_t, std::int64_t> > flow_buckets(const std::vector<NewOrder>& orders) {
    std::vector<std::pair<std::int64_t, std::int64_t> > buckets(60U, std::make_pair(0, 0));
    for (std::size_t i = 0; i < orders.size(); ++i) {
        const std::int64_t second = event_second(orders[i].exchange_time_micros);
        if (second < 300 || second > 600) continue;
        std::size_t index = static_cast<std::size_t>((second - 300) / 5);
        if (index >= buckets.size()) index = buckets.size() - 1U;
        (orders[i].side == kBuy ? buckets[index].first : buckets[index].second) += orders[i].qty;
    }
    return buckets;
}

Maybe flow_slope(const std::vector<NewOrder>& orders) {
    const std::vector<std::pair<std::int64_t, std::int64_t> > buckets = flow_buckets(orders);
    std::vector<std::pair<double, double> > samples;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        Maybe value = imbalance(buckets[i].first, buckets[i].second);
        if (value.valid) samples.push_back(std::make_pair(static_cast<double>(i), value.value));
    }
    return regression_slope(samples);
}

Maybe flow_persistence(const std::vector<NewOrder>& orders) {
    const std::vector<std::pair<std::int64_t, std::int64_t> > buckets = flow_buckets(orders);
    std::int64_t net = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i) net += buckets[i].first - buckets[i].second;
    if (net == 0) return Maybe();
    const int sign = net > 0 ? 1 : -1; std::size_t count = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        const std::int64_t delta = buckets[i].first - buckets[i].second;
        if ((delta > 0 ? 1 : delta < 0 ? -1 : 0) == sign) ++count;
    }
    return Maybe(static_cast<double>(count) / buckets.size());
}

Maybe fleeting(const std::vector<NewOrder>& early, const std::vector<Cancel>& cancels,
               Side side, std::int64_t early_qty) {
    if (early_qty <= 0) return Maybe();
    std::vector<std::int64_t> quantities;
    for (std::size_t i = 0; i < early.size(); ++i) if (early[i].side == side) quantities.push_back(early[i].qty);
    if (quantities.empty()) return Maybe();
    std::sort(quantities.begin(), quantities.end());
    const std::size_t rank = (quantities.size() * 9U + 9U) / 10U;
    const std::int64_t threshold = quantities[rank - 1U];
    std::int64_t value = 0;
    for (std::size_t i = 0; i < cancels.size(); ++i)
        if (cancels[i].side == side && cancels[i].order_original_qty >= threshold &&
            cancels[i].lifetime_micros <= 5000000) value += cancels[i].qty;
    return Maybe(static_cast<double>(value) / early_qty);
}

Maybe fill_rate(const std::vector<OrderState>& orders, Side side, std::int64_t start, std::int64_t end) {
    std::int64_t original = 0, filled = 0;
    for (std::size_t i = 0; i < orders.size(); ++i) {
        const std::int64_t second = event_second(orders[i].exchange_time_micros);
        if (orders[i].side == side && second >= start && second <= end) {
            original += orders[i].original_qty; filled += orders[i].opening_fill_qty;
        }
    }
    return original > 0 ? Maybe(static_cast<double>(filled) / original) : Maybe();
}

Maybe book_depth_imbalance(const PriceBook& book, std::int32_t opening, std::int32_t k) {
    std::int64_t buy = 0, sell = 0;
    for (std::map<std::int32_t, std::int64_t>::const_iterator it = book.buy.lower_bound(opening - k * kQuoteTick);
         it != book.buy.end(); ++it) buy += it->second;
    for (std::map<std::int32_t, std::int64_t>::const_iterator it = book.sell.begin();
         it != book.sell.end() && it->first <= opening + k * kQuoteTick; ++it) sell += it->second;
    return imbalance(buy, sell);
}

Maybe clearing_width(const PriceBook& book, std::int32_t lower, std::int32_t upper,
                     std::int64_t opening_qty) {
    if (book.buy.empty() || book.sell.empty()) return Maybe();
    const std::int32_t lowest_sell = book.sell.begin()->first;
    const std::int32_t highest_buy = book.buy.rbegin()->first;
    const std::int32_t start = ((std::max(lower, lowest_sell) + kQuoteTick - 1) / kQuoteTick) * kQuoteTick;
    const std::int32_t end = std::min(upper, highest_buy) / kQuoteTick * kQuoteTick;
    bool found = false; std::int32_t first = 0, last = 0;
    for (std::int32_t price = start; price <= end; price += kQuoteTick) {
        if (book.match(price) >= 0.99 * opening_qty) {
            if (!found) first = price; last = price; found = true;
        }
    }
    return found ? Maybe(static_cast<double>(last - first) / kQuoteTick) : Maybe();
}

bool impact_reaches(const PriceBook& source, Side side, std::int32_t price,
                    std::int64_t qty, std::int32_t target,
                    std::int32_t lower, std::int32_t upper) {
    PriceBook book = source; book.add(side, price, qty);
    bool unresolved = false; Clearing result = clear_book(book, lower, upper, &unresolved);
    return result.valid && (side == kBuy ? result.price_tick >= target : result.price_tick <= target);
}

bool impact_qty(const PriceBook& book, Side side, std::int32_t opening,
                std::int32_t lower, std::int32_t upper, std::int64_t* output) {
    const std::int32_t target = opening + (side == kBuy ? kQuoteTick : -kQuoteTick);
    if (target > upper || target < lower) return false;
    const std::int32_t price = side == kBuy ? upper : lower;
    const std::int64_t total = book.total(kBuy) + book.total(kSell);
    if (total > (std::numeric_limits<std::int64_t>::max() - 1) / 4) return false;
    const std::int64_t max_search = std::max<std::int64_t>(1, total * 4 + 1);
    std::int64_t high = 1;
    while (high < max_search && !impact_reaches(book, side, price, high, target, lower, upper))
        high = std::min(max_search, high > max_search / 2 ? max_search : high * 2);
    if (!impact_reaches(book, side, price, high, target, lower, upper)) return false;
    std::int64_t low = 1;
    while (low < high) {
        const std::int64_t mid = low + (high - low) / 2;
        if (impact_reaches(book, side, price, mid, target, lower, upper)) high = mid;
        else low = mid + 1;
    }
    *output = low; return true;
}

void compute_factors(AuctionResult* result, const PriceBook& pre_book,
                     const PriceBook& residual_book, const std::vector<OrderState>& states,
                     const std::vector<NewOrder>& orders, const std::vector<Cancel>& cancels) {
    const std::int32_t open = result->observed_auction_price_tick;
    const std::int64_t open_qty = result->observed_auction_qty;
    const std::int64_t demand = pre_book.demand(open), supply = pre_book.supply(open);
    const Maybe final_imb = imbalance(demand, supply);
    set_factor(result, 0, log_return(open, result->pre_close_price_tick));
    set_factor(result, 15, Maybe(std::log1p((open / 1000.0) * open_qty)));
    set_factor(result, 16, final_imb);
    set_factor(result, 17, open_qty > 0 ? divide(demand - supply, open_qty) : Maybe());
    const std::int64_t active_total = pre_book.total(kBuy) + pre_book.total(kSell);
    set_factor(result, 18, active_total > 0 ? Maybe(2.0 * open_qty / active_total) : Maybe());

    const std::vector<GridPoint> grid = build_grid(PriceBook(), result->lower_limit_tick,
                                                    result->upper_limit_tick, open, orders, cancels);
    std::vector<std::pair<std::size_t, Clearing> > full, late, last60;
    const bool full_ok = path_window(grid, 0, 600, &full);
    const bool late_ok = path_window(grid, 300, 600, &late);
    const bool last60_ok = path_window(grid, 540, 600, &last60);
    if (!full_ok || !late_ok || !last60_ok) result->quality_reason_mask |= reason(kPathCoverage);
    if (late_ok) {
        set_factor(result, 1, path_return(late)); set_factor(result, 3, path_slope(late));
        set_factor(result, 5, path_r2(late)); set_factor(result, 6, path_efficiency(late));
        double mae = 0.0;
        for (std::size_t i = 0; i < late.size(); ++i) mae += std::fabs((late[i].second.price_tick - open) / 10.0);
        set_factor(result, 11, Maybe(mae / late.size()));
        std::size_t within = 0;
        for (std::size_t i = 0; i < late.size(); ++i) if (std::abs(late[i].second.price_tick - open) <= 10) ++within;
        set_factor(result, 12, Maybe(static_cast<double>(within) / late.size()));
        set_factor(result, 19, Maybe(std::log1p(static_cast<double>(late.back().second.match_qty)) -
                                     std::log1p(static_cast<double>(late.front().second.match_qty))));
    }
    if (last60_ok) set_factor(result, 2, path_return(last60));
    if (last60_ok) set_factor(result, 4, path_slope(last60));
    if (full_ok) {
        set_factor(result, 7, path_range(full)); set_factor(result, 9, path_drawdown(full));
        set_factor(result, 10, path_end_position(full));
        std::size_t settle = 0;
        bool settled = true;
        for (std::size_t reverse = 0; reverse <= 600U; ++reverse) {
            const std::size_t i = 600U - reverse;
            if (!grid[i].clearing.valid) { settle = 600U - std::min<std::size_t>(600U, i + 1U); settled = false; break; }
            if (std::abs(grid[i].clearing.price_tick - open) > 10) { settle = 600U - i; settled = false; break; }
        }
        if (settled) settle = 600U;
        set_factor(result, 13, Maybe(static_cast<double>(settle)));
        int previous = 0; std::size_t crosses = 0;
        for (std::size_t i = 0; i <= 600U; ++i) {
            if (!grid[i].clearing.valid) { previous = 0; continue; }
            const int delta = grid[i].clearing.price_tick - open;
            if (delta == 0) continue;
            const int sign = delta > 0 ? 1 : -1;
            if (previous != 0 && previous != sign) ++crosses;
            previous = sign;
        }
        set_factor(result, 14, Maybe(static_cast<double>(crosses)));
    }
    if (late_ok) {
        double sumsq = 0.0;
        for (std::size_t i = 301; i <= 600; ++i) if (grid[i - 1].clearing.valid && grid[i].clearing.valid) {
            const double value = std::log(static_cast<double>(grid[i].clearing.price_tick) /
                                          grid[i - 1].clearing.price_tick); sumsq += value * value;
        }
        set_factor(result, 8, Maybe(10000.0 * std::sqrt(sumsq)));
    }
    if (grid.size() > 600U && grid[540].clearing.valid && grid[600].clearing.valid &&
        grid[600].clearing.match_qty > 0)
        set_factor(result, 20, divide(grid[600].clearing.match_qty - grid[540].clearing.match_qty,
                                      grid[600].clearing.match_qty));
    std::vector<Maybe> late_imbalances; std::vector<std::pair<double, double> > late_samples;
    for (std::size_t i = 300; i <= 600; ++i) {
        Maybe value = imbalance(grid[i].anchor_buy, grid[i].anchor_sell); late_imbalances.push_back(value);
        if (value.valid) late_samples.push_back(std::make_pair(i / 60.0, value.value));
    }
    double im_sum = 0.0; std::size_t im_count = 0;
    for (std::size_t i = 0; i < late_imbalances.size(); ++i) if (late_imbalances[i].valid) { im_sum += late_imbalances[i].value; ++im_count; }
    set_factor(result, 21, im_count ? Maybe(im_sum / im_count) : Maybe());
    set_factor(result, 22, final_imb); set_factor(result, 23, regression_slope(late_samples));
    if (grid.size() > 600U) {
        Maybe start = imbalance(grid[540].anchor_buy, grid[540].anchor_sell);
        Maybe end = imbalance(grid[600].anchor_buy, grid[600].anchor_sell);
        set_factor(result, 24, start.valid && end.valid ? Maybe(end.value - start.value) : Maybe());
    }

    const std::vector<NewOrder> full_orders = order_window(orders, 0, 600);
    const std::vector<NewOrder> early_orders = order_window(orders, 0, 299);
    const std::vector<NewOrder> late_orders = order_window(orders, 300, 600);
    const std::vector<NewOrder> last_orders = order_window(orders, 540, 600);
    set_factor(result, 25, order_imbalance(full_orders)); set_factor(result, 26, order_imbalance(late_orders));
    set_factor(result, 27, order_imbalance(last_orders));
    const std::pair<std::int64_t, std::int64_t> early_qty = side_quantities(early_orders);
    const std::pair<std::int64_t, std::int64_t> late_qty = side_quantities(late_orders);
    set_factor(result, 28, Maybe(std::log1p(static_cast<double>(late_qty.first)) -
                                 std::log1p(static_cast<double>(early_qty.first)) -
                                 std::log1p(static_cast<double>(late_qty.second)) +
                                 std::log1p(static_cast<double>(early_qty.second))));
    set_factor(result, 29, marketable_imbalance(late_orders, open));
    set_factor(result, 30, marketable_imbalance(last_orders, open));
    set_factor(result, 31, aggression_mean(late_orders, kBuy, open));
    set_factor(result, 32, aggression_mean(late_orders, kSell, open));
    set_factor(result, 33, arrival_gap(late_orders)); set_factor(result, 34, flow_slope(late_orders));
    set_factor(result, 35, flow_persistence(late_orders));
    std::int64_t cancel_buy = 0, cancel_sell = 0, exec_buy = 0, exec_sell = 0;
    for (std::size_t i = 0; i < cancels.size(); ++i) {
        (cancels[i].side == kBuy ? cancel_buy : cancel_sell) += cancels[i].qty;
        if (cancels[i].side == kBuy && cancels[i].price_tick >= open) exec_buy += cancels[i].qty;
        if (cancels[i].side == kSell && cancels[i].price_tick <= open) exec_sell += cancels[i].qty;
    }
    set_factor(result, 36, early_qty.first > 0 ? divide(cancel_buy, early_qty.first) : Maybe());
    set_factor(result, 37, early_qty.second > 0 ? divide(cancel_sell, early_qty.second) : Maybe());
    set_factor(result, 38, imbalance(cancel_sell, cancel_buy)); set_factor(result, 39, imbalance(exec_sell, exec_buy));
    set_factor(result, 40, fleeting(early_orders, cancels, kBuy, early_qty.first));
    set_factor(result, 41, fleeting(early_orders, cancels, kSell, early_qty.second));
    const std::pair<std::int64_t, std::int64_t> anchor = anchor_before(open, orders, cancels);
    std::int64_t max_buy = 0, max_sell = 0;
    for (std::size_t i = 0; i < 300U; ++i) { max_buy = std::max(max_buy, grid[i].anchor_buy); max_sell = std::max(max_sell, grid[i].anchor_sell); }
    if (max_buy > 0 && max_sell > 0)
        set_factor(result, 42, Maybe(static_cast<double>(max_sell - anchor.second) / max_sell -
                                     static_cast<double>(max_buy - anchor.first) / max_buy));
    const Maybe fill_bl = fill_rate(states, kBuy, 300, 600), fill_sl = fill_rate(states, kSell, 300, 600);
    const Maybe fill_be = fill_rate(states, kBuy, 0, 299), fill_se = fill_rate(states, kSell, 0, 299);
    set_factor(result, 43, fill_bl); set_factor(result, 44, fill_sl);
    set_factor(result, 45, fill_bl.valid && fill_be.valid ? Maybe(fill_bl.value - fill_be.value) : Maybe());
    set_factor(result, 46, fill_sl.valid && fill_se.valid ? Maybe(fill_sl.value - fill_se.value) : Maybe());
    set_factor(result, 47, final_imb); set_factor(result, 48, book_depth_imbalance(pre_book, open, 3));
    set_factor(result, 49, book_depth_imbalance(pre_book, open, 10));
    set_factor(result, 50, book_depth_imbalance(residual_book, open, 1));
    set_factor(result, 51, book_depth_imbalance(residual_book, open, 5));
    if (residual_book.buy.empty() || residual_book.sell.empty()) result->quality_reason_mask |= reason(kOneSidedResidualBook);
    else {
        const std::int32_t bid = residual_book.buy.rbegin()->first, ask = residual_book.sell.begin()->first;
        set_factor(result, 52, Maybe((ask - bid) / 10.0));
        set_factor(result, 53, Maybe(((ask + bid) / 2.0 - open) / 10.0));
    }
    const std::int64_t at_buy = residual_book.buy.count(open) ? residual_book.buy.find(open)->second : 0;
    const std::int64_t at_sell = residual_book.sell.count(open) ? residual_book.sell.find(open)->second : 0;
    set_factor(result, 54, imbalance(at_buy, at_sell));
    if (open_qty > 0) {
        const Maybe loss_up = divide(open_qty - pre_book.match(open + 10), open_qty);
        const Maybe loss_down = divide(open_qty - pre_book.match(open - 10), open_qty);
        set_factor(result, 55, loss_up); set_factor(result, 56, loss_down);
        set_factor(result, 57, Maybe(loss_up.value - loss_down.value));
    }
    set_factor(result, 58, clearing_width(pre_book, result->lower_limit_tick,
                                           result->upper_limit_tick, open_qty));
    if (open <= result->lower_limit_tick || open >= result->upper_limit_tick)
        result->quality_reason_mask |= reason(kPriceBoundary);
    std::int64_t up = 0, down = 0;
    const bool have_up = impact_qty(pre_book, kBuy, open, result->lower_limit_tick, result->upper_limit_tick, &up);
    const bool have_down = impact_qty(pre_book, kSell, open, result->lower_limit_tick, result->upper_limit_tick, &down);
    if (!have_up || !have_down) result->quality_reason_mask |= reason(kCounterfactualUnreachable);
    if (open_qty > 0) {
        if (have_up) set_factor(result, 59, divide(up, open_qty));
        if (have_down) set_factor(result, 60, divide(down, open_qty));
        if (have_up && have_down) set_factor(result, 61, divide(down - up, open_qty));
    }
}

bool exact_order_id(const sse_live::TickEvent& event, Side* side, std::uint64_t* id) {
    if (event.side == 0 && event.buy_order_no > 0 && event.sell_order_no == 0) {
        *side = kBuy; *id = event.buy_order_no; return true;
    }
    if (event.side == 1 && event.sell_order_no > 0 && event.buy_order_no == 0) {
        *side = kSell; *id = event.sell_order_no; return true;
    }
    return false;
}

bool shares(std::uint64_t raw, std::int64_t* output) {
    if (raw == 0 || raw % 1000ULL != 0 || raw / 1000ULL > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
    *output = static_cast<std::int64_t>(raw / 1000ULL); return true;
}

}  // namespace

struct AuctionAccumulator::Impl {
    std::string security;
    StaticMetadata metadata;
    AuctionResult result;
    std::int32_t lower, upper;
    bool done, opening_started;
    std::uint64_t status_arrival;
    std::unordered_map<std::uint64_t, OrderState> orders;
    std::vector<NewOrder> new_orders;
    std::vector<Cancel> cancels;
    std::vector<Trade> trades;
    PriceBook pre_book;
    std::map<std::pair<char, std::uint32_t>, std::uint64_t> last_seq;
    std::int64_t valid_full, total_full, valid_late, total_late;

    Impl(const std::string& id, const StaticMetadata& value)
        : security(id), metadata(value), lower(price_tick(value.lower_limit)),
          upper(price_tick(value.upper_limit)), done(false), opening_started(false),
          status_arrival(0), valid_full(0), total_full(0), valid_late(0), total_late(0) {
        result.security_id = id; result.source_present = false;
        result.pre_close_price_tick = price_tick(value.pre_close);
        if (value.is_ipo_first_day &&
            lower == ratio_quote_tick(result.pre_close_price_tick, 64, 100) &&
            upper == ratio_quote_tick(result.pre_close_price_tick, 144, 100)) {
            lower = ratio_quote_tick(result.pre_close_price_tick, 80, 100);
            upper = ratio_quote_tick(result.pre_close_price_tick, 120, 100);
        }
        result.lower_limit_tick = lower; result.upper_limit_tick = upper;
        if (!value.auction_static_ready() || lower <= 0 || upper < lower || result.pre_close_price_tick <= 0)
            result.quality_reason_mask |= reason(kStaticDataMissing);
    }
};

AuctionResult::AuctionResult()
    : source_present(false), has_status(false), has_auction_match(false), hard_valid(false),
      status_channel_no(0), opening_auction_last_app_seq(0), quality_reason_mask(0),
      canonical_valid_mask(0), projected_valid_mask(0), source_order_rows(0),
      source_trade_rows(0), conservation_error_count(0), pre_close_price_tick(0),
      lower_limit_tick(0), upper_limit_tick(0), observed_auction_price_tick(0),
      observed_auction_qty(0), reconstructed_auction_price_tick(0),
      reconstructed_auction_qty(0), valid_price_share_full(canonical_nan()),
      valid_price_share_late(canonical_nan()) {
    std::fill(canonical_factors, canonical_factors + kCanonicalFactorCount, canonical_nan());
    std::fill(factors, factors + kAuction59FactorCount, canonical_nan());
}

AuctionAccumulator::AuctionAccumulator(const std::string& id, const StaticMetadata& metadata)
    : impl_(new Impl(id, metadata)) {}
AuctionAccumulator::~AuctionAccumulator() { delete impl_; }
bool AuctionAccumulator::has_status() const { return impl_->result.has_status; }
bool AuctionAccumulator::finalized() const { return impl_->done; }

void AuctionAccumulator::observe(const sse_live::TickEvent& event, std::uint64_t realtime_ns,
                                 std::uint64_t arrival_index) {
    Impl& s = *impl_;
    if (s.done) return;
    s.result.source_present = true;
    const std::uint64_t time = event.time_of_day_micros % kDayMicros;
    if (time >= kAuctionStart && time < kContinuousStart) {
        const char kind = event.event_type == 'T' ? 'T' : 'O';
        const std::pair<char, std::uint32_t> key(kind, event.channel_no);
        std::map<std::pair<char, std::uint32_t>, std::uint64_t>::iterator previous = s.last_seq.find(key);
        if (previous != s.last_seq.end() && event.app_seq_num <= previous->second)
            s.result.quality_reason_mask |= reason(kDuplicateOrNoncausalSeq);
        s.last_seq[key] = event.app_seq_num;
    }
    if (event.event_type == 'S') {
        ++s.result.source_order_rows;
        const bool zero = event.buy_order_no == 0 && event.sell_order_no == 0 &&
                          event.price_raw == 0 && event.quantity_raw == 0 && event.amount_raw == 0;
        if (!zero || s.result.has_status) s.result.quality_reason_mask |= reason(kOpeningBoundaryConflict);
        if (zero && !s.result.has_status) {
            s.result.has_status = true; s.result.status_channel_no = event.channel_no;
            s.result.opening_auction_last_app_seq = event.app_seq_num > 0 ? event.app_seq_num - 1 : 0;
            s.status_arrival = arrival_index;
        }
        return;
    }
    if (event.event_type == 'T') {
        if (time >= kAuctionStart && time < kContinuousStart) ++s.result.source_trade_rows;
        if (time >= kAuctionStart && time < kAuctionEnd && event.quantity_raw > 0)
            s.result.quality_reason_mask |= reason(kAbnormalTrade);
        if (time >= kAuctionEnd && time < kContinuousStart) {
            std::int64_t qty = 0;
            if (!shares(event.quantity_raw, &qty) || event.price_raw == 0 ||
                event.buy_order_no == 0 || event.sell_order_no == 0) {
                s.result.quality_reason_mask |= reason(kAbnormalTrade); return;
            }
            Trade trade = {event.app_seq_num, event.time_of_day_micros, realtime_ns, arrival_index,
                           event.channel_no, static_cast<std::int32_t>(event.price_raw), qty,
                           event.buy_order_no, event.sell_order_no};
            s.trades.push_back(trade); s.opening_started = true;
        }
        return;
    }
    if (event.event_type != 'A' && event.event_type != 'D') {
        if (time >= kAuctionStart && time <= kAuctionEnd) s.result.quality_reason_mask |= reason(kUnknownOrderType);
        return;
    }
    if (time >= kAuctionStart && time < kContinuousStart) ++s.result.source_order_rows;
    if (s.opening_started && time >= kAuctionStart && time <= kAuctionEnd)
        s.result.quality_reason_mask |= reason(kOpeningBoundaryConflict);
    if (time < kAuctionStart || time > kAuctionEnd) return;
    Side side; std::uint64_t id = 0; std::int64_t qty = 0;
    if (!exact_order_id(event, &side, &id) || !shares(event.quantity_raw, &qty)) {
        s.result.quality_reason_mask |= reason(event.event_type == 'A' ? kUnknownOrderType : kAbnormalCancel);
        return;
    }
    if (event.event_type == 'A') {
        s.total_full += qty; if (time >= kCancelEnd) s.total_late += qty;
        const bool valid_price = event.price_raw >= static_cast<std::uint32_t>(std::max(0, s.lower)) &&
                                 event.price_raw <= static_cast<std::uint32_t>(std::max(0, s.upper)) &&
                                 event.price_raw % kQuoteTick == 0;
        if (valid_price) { s.valid_full += qty; if (time >= kCancelEnd) s.valid_late += qty; }
        if (!valid_price || qty <= 0) { s.result.quality_reason_mask |= reason(kPriceBoundary); return; }
        if (s.orders.find(id) != s.orders.end()) { s.result.quality_reason_mask |= reason(kDuplicateOrNoncausalSeq); return; }
        OrderState state = {event.app_seq_num, event.time_of_day_micros, side,
                            static_cast<std::int32_t>(event.price_raw), qty, 0, 0, qty};
        s.orders[id] = state;
        NewOrder order = {id, event.app_seq_num, event.time_of_day_micros, arrival_index, side,
                          static_cast<std::int32_t>(event.price_raw), qty};
        s.new_orders.push_back(order); s.pre_book.add(side, order.price_tick, qty);
    } else {
        if (time >= kCancelEnd) { s.result.quality_reason_mask |= reason(kAbnormalCancel); return; }
        std::unordered_map<std::uint64_t, OrderState>::iterator found = s.orders.find(id);
        if (found == s.orders.end() || found->second.residual_qty <= 0 || found->second.residual_qty != qty ||
            !s.pre_book.remove(found->second.side, found->second.price_tick, qty)) {
            s.result.quality_reason_mask |= reason(kAbnormalCancel); return;
        }
        OrderState& state = found->second;
        state.cancelled_qty += qty; state.residual_qty = 0;
        Cancel cancel = {event.app_seq_num, event.time_of_day_micros, arrival_index, state.side,
                         state.price_tick, qty, state.original_qty,
                         static_cast<std::int64_t>(event.time_of_day_micros - state.exchange_time_micros)};
        s.cancels.push_back(cancel);
    }
}

AuctionResult AuctionAccumulator::finalize() {
    Impl& s = *impl_;
    if (s.done) return s.result;
    s.done = true;
    if (!s.result.source_present) s.result.quality_reason_mask |= reason(kSourceMissing);
    if (!s.result.has_status || s.result.opening_auction_last_app_seq == 0)
        s.result.quality_reason_mask |= reason(kOpeningBoundaryConflict);
    s.result.valid_price_share_full = s.total_full > 0 ? static_cast<float>(s.valid_full) / s.total_full : canonical_nan();
    s.result.valid_price_share_late = s.total_late > 0 ? static_cast<float>(s.valid_late) / s.total_late : canonical_nan();
    std::vector<Trade> opening;
    for (std::size_t i = 0; i < s.trades.size(); ++i) {
        if (s.result.has_status && s.trades[i].channel_no == s.result.status_channel_no &&
            s.trades[i].app_seq <= s.result.opening_auction_last_app_seq &&
            s.trades[i].arrival_index < s.status_arrival) opening.push_back(s.trades[i]);
    }
    std::sort(opening.begin(), opening.end(), [](const Trade& a, const Trade& b) {
        return a.app_seq != b.app_seq ? a.app_seq < b.app_seq : a.arrival_index < b.arrival_index;
    });
    if (opening.empty()) s.result.quality_reason_mask |= reason(kNoAuctionMatch);
    else {
        s.result.has_auction_match = true; s.result.observed_auction_price_tick = opening[0].price_tick;
        std::set<std::int32_t> prices;
        for (std::size_t i = 0; i < opening.size(); ++i) {
            prices.insert(opening[i].price_tick); s.result.observed_auction_qty += opening[i].qty;
            std::unordered_map<std::uint64_t, OrderState>::iterator buy = s.orders.find(opening[i].buy_order_id);
            std::unordered_map<std::uint64_t, OrderState>::iterator sell = s.orders.find(opening[i].sell_order_id);
            if (buy == s.orders.end() || buy->second.side != kBuy || buy->second.residual_qty < opening[i].qty)
                s.result.quality_reason_mask |= reason(kAbnormalTrade);
            else { buy->second.residual_qty -= opening[i].qty; buy->second.opening_fill_qty += opening[i].qty; }
            if (sell == s.orders.end() || sell->second.side != kSell || sell->second.residual_qty < opening[i].qty)
                s.result.quality_reason_mask |= reason(kAbnormalTrade);
            else { sell->second.residual_qty -= opening[i].qty; sell->second.opening_fill_qty += opening[i].qty; }
        }
        if (prices.size() != 1U) s.result.quality_reason_mask |= reason(kMultipleOpeningPrices);
    }
    PriceBook residual;
    std::vector<OrderState> states; states.reserve(s.orders.size());
    for (std::unordered_map<std::uint64_t, OrderState>::const_iterator it = s.orders.begin(); it != s.orders.end(); ++it) {
        const OrderState& order = it->second;
        if (order.original_qty != order.cancelled_qty + order.opening_fill_qty + order.residual_qty)
            ++s.result.conservation_error_count;
        if (order.residual_qty > 0) residual.add(order.side, order.price_tick, order.residual_qty);
        states.push_back(order);
    }
    std::sort(states.begin(), states.end(), [](const OrderState& a, const OrderState& b) { return a.app_seq < b.app_seq; });
    if (s.result.conservation_error_count) s.result.quality_reason_mask |= reason(kOrderConservation);
    bool unresolved = false;
    const Clearing reconstructed = clear_book(s.pre_book, s.lower, s.upper, &unresolved);
    if (reconstructed.valid) {
        s.result.reconstructed_auction_price_tick = reconstructed.price_tick;
        s.result.reconstructed_auction_qty = reconstructed.match_qty;
    } else if (unresolved) s.result.quality_reason_mask |= reason(kClearingTieUnresolved);
    else s.result.quality_reason_mask |= reason(kNoAuctionMatch);
    if (s.result.observed_auction_price_tick > 0 &&
        s.result.observed_auction_price_tick != s.result.reconstructed_auction_price_tick)
        s.result.quality_reason_mask |= reason(kReconstructedPriceMismatch);
    if (s.result.observed_auction_qty > 0 &&
        s.result.observed_auction_qty != s.result.reconstructed_auction_qty)
        s.result.quality_reason_mask |= reason(kReconstructedQtyMismatch);
    const std::uint64_t fatal = reason(kSuspended) | reason(kSourceMissing) | reason(kStaticDataMissing) |
        reason(kMetaConflict) | reason(kOpeningBoundaryConflict) | reason(kUnknownOrderType) |
        reason(kDuplicateOrNoncausalSeq) | reason(kAbnormalCancel) | reason(kAbnormalTrade) |
        reason(kOrderConservation) | reason(kNoAuctionMatch) | reason(kMultipleOpeningPrices) |
        reason(kClearingTieUnresolved) | reason(kReconstructedPriceMismatch) |
        reason(kReconstructedQtyMismatch) | reason(kPriceBoundary);
    s.result.hard_valid = (s.result.quality_reason_mask & fatal) == 0 &&
        s.result.has_auction_match && reconstructed.valid && reconstructed.match_qty > 0;
    if (s.result.hard_valid) compute_factors(&s.result, s.pre_book, residual, states, s.new_orders, s.cancels);
    static const std::size_t source_bits[kAuction59FactorCount] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,19,20,21,23,24,25,26,27,28,29,30,31,
        32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,48,49,50,51,52,53,54,55,56,57,58,59,60,61};
    if (s.result.hard_valid) for (std::size_t i = 0; i < kAuction59FactorCount; ++i) {
        s.result.factors[i] = s.result.canonical_factors[source_bits[i]];
        if (s.result.canonical_valid_mask & (1ULL << source_bits[i])) s.result.projected_valid_mask |= 1ULL << i;
    }
    return s.result;
}

const char* quality_reason_name(std::size_t bit) {
    static const char* names[] = {"SUSPENDED","SOURCE_MISSING","STATIC_DATA_MISSING","META_WINDDB_CONFLICT",
        "OPENING_BOUNDARY_CONFLICT","UNKNOWN_ORDER_TYPE","DUPLICATE_OR_NONCAUSAL_SEQ","ABNORMAL_CANCEL",
        "ABNORMAL_TRADE","ORDER_CONSERVATION","NO_AUCTION_MATCH","MULTIPLE_OPENING_PRICES",
        "CLEARING_TIE_UNRESOLVED","RECONSTRUCTED_PRICE_MISMATCH","RECONSTRUCTED_QTY_MISMATCH",
        "PATH_COVERAGE","ONE_SIDED_RESIDUAL_BOOK","PRICE_BOUNDARY","COUNTERFACTUAL_UNREACHABLE","NUMERIC_OVERFLOW"};
    return bit < sizeof(names) / sizeof(names[0]) ? names[bit] : "UNKNOWN";
}

const char* auction59_factor_name(std::size_t bit) {
    static const char* names[] = {"FNL_GAP","PRC_RET_LATE","PRC_RET_LAST60","PRC_SLOPE_LATE",
        "PRC_SLOPE_LAST60","PRC_R2_LATE","PRC_EFF_LATE","PRC_RANGE_FULL","PRC_RV_LATE",
        "PRC_MDD_FULL","PRC_END_POSITION_FULL","CNV_MAE_LATE","CNV_WITHIN_1_LATE","CNV_SETTLE_1",
        "CNV_CROSS_COUNT","FNL_MATCH_AMT","FNL_IMB","FNL_ACTIVE_MATCH_RATE","MAT_GROWTH_LATE",
        "MAT_LAST60_CHANGE","ANK_IMB_MEAN_LATE","ANK_IMB_SLOPE_LATE","ANK_IMB_SHOCK60",
        "ORD_Q_IMB_FULL","ORD_Q_IMB_LATE","ORD_Q_IMB_LAST60","ORD_NET_ACCEL",
        "AGR_MARKETABLE_IMB_LATE","AGR_MARKETABLE_IMB_LAST60","AGR_MEAN_BUY_LATE",
        "AGR_MEAN_SELL_LATE","TIM_BUY_SELL_GAP_LATE","FLW_SLOPE_LATE","FLW_PERSIST_LATE",
        "CXL_RATE_BUY","CXL_RATE_SELL","CXL_NET_PRESSURE","CXL_EXEC_PRESSURE","AGE_FLEETING_BUY",
        "AGE_FLEETING_SELL","FAD_NET","LIF_FILL_RATE_BUY_LATE","LIF_FILL_RATE_SELL_LATE",
        "LIF_LATE_FILL_ADV_BUY","LIF_LATE_FILL_ADV_SELL","BOK_PRE_IMB_K3","BOK_PRE_IMB_K10",
        "RES_IMB_K1","RES_IMB_K5","RES_SPREAD","RES_MID_OFFSET","RES_AT_OPEN_IMB","CRV_LOSS_UP_K1",
        "CRV_LOSS_DOWN_K1","CRV_LOSS_ASYM_K1","CRV_WIDTH_99","IMP_Q_UP1","IMP_Q_DOWN1","IMP_FRAGILITY_ASYM"};
    return bit < sizeof(names) / sizeof(names[0]) ? names[bit] : "UNKNOWN";
}

}  // namespace sse_auction59
