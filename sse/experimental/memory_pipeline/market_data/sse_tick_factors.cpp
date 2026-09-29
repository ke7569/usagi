#include "sse_tick_factors.h"
#include "sse_tick_units.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace sse_tick {
namespace {

// price() converts EFH raw milli-yuan to canonical yuan.  Order-book and
// flow quantities are stored in canonical shares (see sse_tick_units.h), so
// no further quantity scaling belongs in the factor layer.

std::uint64_t factor_clock_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* const kNames[kTickFactorCount] = {
    "factor_hermes_permille", "factor_tr_sqrt_positive", "factor_spread_permille",
    "factor_mid_return_permille", "factor_fee_on_tick", "factor_bid_volume_change_ratio",
    "factor_ask_volume_change_ratio", "factor_weighted_return_permille_1",
    "factor_weighted_return_permille_2", "factor_weighted_return_permille_3",
    "factor_weighted_return_permille_4", "factor_weighted_return_permille_5",
    "factor_weighted_ask_permille", "factor_weighted_bid_permille",
    "factor_weighted_ask_return_permille", "factor_weighted_bid_return_permille",
    "factor_weighted_volume_imbalance", "factor_volume_imbalance", "factor_percent_turnover",
    "factor_liquidity_ask_l1_share", "factor_liquidity_bid_l1_share",
    "factor_positive_order_flow", "factor_negative_order_flow", "factor_market_flow",
    "factor_cancel_buy_flow", "factor_cancel_sell_flow", "factor_positive_trade",
    "factor_negative_trade", "factor_positive_fill_rate", "factor_negative_fill_rate",
    "factor_order_flow_imbalance", "factor_cfr_imbalance",
    "factor_book_fixdist_imbalance_1pct", "factor_book_fixdist_imbalance_5pct",
    "factor_book_fixdist_weighted_1pct", "factor_book_fixdist_weighted_5pct",
    "factor_book_avg_size_imbalance", "factor_book_avg_size_imbalance_l1",
    "factor_book_avg_size_imbalance_l5", "factor_book_count_imbalance",
    "factor_book_count_imbalance_l1", "factor_book_count_imbalance_l5",
    "factor_book_life_imbalance", "factor_book_life_imbalance_l1",
    "factor_book_life_imbalance_l5", "factor_max_bid_distance_ratio",
    "factor_max_ask_distance_ratio", "factor_max_vol_distance_imbalance",
    "factor_book_young_imbalance_1pct", "factor_book_fixdist_hermes"};

double safe_div(double a, double b) { return b == 0.0 ? 0.0 : a / b; }
double finite_or_zero(double x) { return std::isfinite(x) ? x : 0.0; }
double price(const Level& level) { return static_cast<double>(level.price_raw) / 1000.0; }

double mid_price(const Level* bid, const Level* ask, double last) {
    if (bid && ask && bid->quantity && ask->quantity && bid->price_raw && ask->price_raw)
        return (price(*bid) + price(*ask)) * 0.5;
    return last > 0.0 ? last : 0.0;
}

double weighted_price(const Level* levels, std::size_t n, bool ask, double mid) {
    double dot = 0.0, volume = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double q = static_cast<double>(levels[i].quantity);
        dot += price(levels[i]) * q;
        volume += q;
    }
    if (volume == 0.0) return 0.0;
    const double value = dot / volume;
    return ask ? value - mid : mid - value;
}

double weighted_book_price(const Level* bids, const Level* asks, std::size_t n,
                           double* bid_volume, double* ask_volume) {
    double dot = 0.0, volume = 0.0, bv = 0.0, av = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double b = static_cast<double>(bids[i].quantity);
        const double a = static_cast<double>(asks[i].quantity);
        dot += price(bids[i]) * b + price(asks[i]) * a;
        bv += b; av += a; volume += b + a;
    }
    if (bid_volume) *bid_volume = bv;
    if (ask_volume) *ask_volume = av;
    return volume == 0.0 ? 0.0 : dot / volume;
}

double classic_hermes(const Level* bids, const Level* asks, std::size_t n,
                      double fallback) {
    if (n == 0 || bids[0].quantity == 0 || asks[0].quantity == 0) return fallback;
    double sum = 0.0, weight_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (bids[i].quantity == 0 || asks[i].quantity == 0) break;
        const double bv = static_cast<double>(bids[i].quantity);
        const double av = static_cast<double>(asks[i].quantity);
        const double weight = static_cast<double>(5U - i);
        sum += (price(asks[i]) * bv + price(bids[i]) * av) / (av + bv) * weight;
        weight_sum += weight;
    }
    return weight_sum == 0.0 ? fallback : sum / weight_sum;
}

struct Band {
    double volume;
    double count;
    double time_sum;
    double amount;
    double young_volume;
    double max_volume;
    double max_price;
    Band() : volume(0), count(0), time_sum(0), amount(0), young_volume(0),
             max_volume(0), max_price(0) {}
};

struct SideBands {
    Band dist01;
    Band dist05;
    Band dist10;
    Band top1;
    Band top5;
    double young_weighted;
    double fix_dot;
    double fix_weight;
    double max_volume;
    double max_price;
    SideBands() : dist01(), dist05(), dist10(), top1(), top5(),
                  young_weighted(0.0), fix_dot(0.0), fix_weight(0.0),
                  max_volume(0.0), max_price(0.0) {}
};

void add_level_to_band(const Level& level, double* volume, double* count,
                       double* time_sum, double* amount, double* young) {
    const double q = static_cast<double>(level.quantity);
    *volume += q;
    *count += static_cast<double>(level.order_count);
    *time_sum += static_cast<double>(level.add_time_sum_micros);
    *amount += price(level) * q;
    *young += static_cast<double>(level.young_quantity);
}

Band make_band(const std::vector<Level>& levels, bool ask, double mid, double dist,
               std::size_t max_levels) {
    Band band;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const double p = price(levels[i]);
        const bool in_dist = ask ? p < mid * (1.0 + dist) : p > mid * (1.0 - dist);
        const bool in_levels = i < max_levels;
        if (in_dist) add_level_to_band(levels[i], &band.volume, &band.count,
                                       &band.time_sum, &band.amount,
                                       &band.young_volume);
        if (in_dist && levels[i].quantity > band.max_volume) {
            band.max_volume = static_cast<double>(levels[i].quantity);
            band.max_price = p;
        }
        if (in_levels) {
            // Top-N bands are represented by the same accumulator but are
            // created separately by the caller.
        }
    }
    return band;
}

Band make_top_band(const std::vector<Level>& levels, std::size_t max_levels) {
    Band band;
    for (std::size_t i = 0; i < levels.size() && i < max_levels; ++i)
        add_level_to_band(levels[i], &band.volume, &band.count, &band.time_sum,
                          &band.amount, &band.young_volume);
    return band;
}

// Aggregate all distance bands and top-N statistics in one ordered pass. The
// old implementation rescanned each side six times and then rescanned it for
// maximum/young-order/hermes terms; keeping these accumulators together makes
// the common full-depth path linear with a small constant.
SideBands aggregate_side(const std::vector<Level>& levels, bool ask,
                         double mid) {
    SideBands result;
    if (mid <= 0.0) return result;
    const double max01 = mid * 0.01;
    const double max05 = mid * 0.05;
    const double max10 = mid * 0.10;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const Level& level = levels[i];
        const double p = price(level);
        const double q = static_cast<double>(level.quantity);
        if (i == 0U) add_level_to_band(level, &result.top1.volume,
                                       &result.top1.count, &result.top1.time_sum,
                                       &result.top1.amount, &result.top1.young_volume);
        if (i < 5U) add_level_to_band(level, &result.top5.volume,
                                      &result.top5.count, &result.top5.time_sum,
                                      &result.top5.amount, &result.top5.young_volume);
        if (q > result.max_volume) {
            result.max_volume = q;
            result.max_price = p;
        }
        const double d = ask ? p - mid : mid - p;
        // make_band uses strict distance bounds; young/hermes use inclusive
        // bounds (their reference loops break only when d > max).
        if (d < max01) {
            add_level_to_band(level, &result.dist01.volume, &result.dist01.count,
                              &result.dist01.time_sum, &result.dist01.amount,
                              &result.dist01.young_volume);
        }
        if (d >= 0.0 && d <= max01)
            result.young_weighted += static_cast<double>(level.young_quantity) *
                                     (1.0 - d / max01);
        if (d < max05) {
            add_level_to_band(level, &result.dist05.volume, &result.dist05.count,
                              &result.dist05.time_sum, &result.dist05.amount,
                              &result.dist05.young_volume);
        }
        if (d >= 0.0 && d <= max05) {
            const double w = (1.0 - d / max05) * q;
            if (w > 0.0) {
                result.fix_dot += w * p;
                result.fix_weight += w;
            }
        }
        if (d < max10)
            add_level_to_band(level, &result.dist10.volume, &result.dist10.count,
                              &result.dist10.time_sum, &result.dist10.amount,
                              &result.dist10.young_volume);
    }
    return result;
}

double avg_size_imbalance(const Band& ask, const Band& bid) {
    if (ask.count <= 0.0 || bid.count <= 0.0) return 0.0;
    const double av = ask.volume / ask.count;
    const double bv = bid.volume / bid.count;
    return safe_div(av - bv, av + bv);
}

double count_imbalance(const Band& ask, const Band& bid) {
    return safe_div(ask.count - bid.count, ask.count + bid.count);
}

double life_imbalance(const Band& ask, const Band& bid, double now) {
    if (ask.count <= 0.0 || bid.count <= 0.0) return 0.0;
    const double al = now - ask.time_sum / ask.count;
    const double bl = now - bid.time_sum / bid.count;
    return safe_div(al - bl, al + bl);
}

double fix_imbalance(const Band& ask, const Band& bid) {
    return safe_div(ask.volume - bid.volume, ask.volume + bid.volume + 1.0);
}

double weighted_fix_imbalance(const Band& ask, const Band& bid, double mid,
                              double dist) {
    const double max_distance = mid * dist;
    if (max_distance <= 0.0) return 0.0;
    const double av = ask.volume * (1.0 + mid / max_distance) - ask.amount / max_distance;
    const double bv = bid.volume * (1.0 - mid / max_distance) + bid.amount / max_distance;
    return safe_div(av - bv, av + bv);
}

double max_vol_distance_imbalance(double ask_max, double bid_max, double mid) {
    if (mid <= 0.0 || ask_max <= 0.0 || bid_max <= 0.0) return 0.0;
    const double ad = ask_max / mid - 1.0;
    const double bd = -(bid_max / mid - 1.0);
    return safe_div(ad - bd, ad + bd);
}

double young_imbalance(const std::vector<Level>& asks, const std::vector<Level>& bids,
                       double mid, double dist) {
    if (mid <= 0.0) return 0.0;
    const double max_distance = mid * dist;
    double av = 0.0, bv = 0.0;
    for (std::size_t i = 0; i < bids.size(); ++i) {
        const double d = mid - price(bids[i]);
        if (d < 0.0) continue;
        if (d > max_distance) break;
        bv += static_cast<double>(bids[i].young_quantity) * (1.0 - d / max_distance);
    }
    for (std::size_t i = 0; i < asks.size(); ++i) {
        const double d = price(asks[i]) - mid;
        if (d < 0.0) continue;
        if (d > max_distance) break;
        av += static_cast<double>(asks[i].young_quantity) * (1.0 - d / max_distance);
    }
    return safe_div(av - bv, av + bv);
}

double fix_dist_hermes(const std::vector<Level>& asks, const std::vector<Level>& bids,
                       double mid, double dist) {
    if (mid <= 0.0 || asks.empty() || bids.empty()) return 0.0;
    const double max_distance = mid * dist;
    double ask_dot = 0.0, ask_weight = 0.0, bid_dot = 0.0, bid_weight = 0.0;
    for (std::size_t i = 0; i < bids.size(); ++i) {
        const double d = mid - price(bids[i]);
        if (d < 0.0) continue;
        if (d > max_distance) break;
        const double w = (1.0 - d / max_distance) * bids[i].quantity;
        if (w <= 0.0) continue;
        bid_dot += w * price(bids[i]); bid_weight += w;
    }
    for (std::size_t i = 0; i < asks.size(); ++i) {
        const double d = price(asks[i]) - mid;
        if (d < 0.0) continue;
        if (d > max_distance) break;
        const double w = (1.0 - d / max_distance) * asks[i].quantity;
        if (w <= 0.0) continue;
        ask_dot += w * price(asks[i]); ask_weight += w;
    }
    const double effective_bid = bid_weight > 0.0 ? bid_dot / bid_weight : price(bids[0]);
    const double effective_ask = ask_weight > 0.0 ? ask_dot / ask_weight : price(asks[0]);
    const double hermes = (effective_bid + effective_ask) * 0.5;
    return hermes > 0.0 ? std::max(-5.0, std::min(5.0, (hermes / mid - 1.0) * 1000.0)) : 0.0;
}

double weighted_return(const Level* previous, const Level* current, std::size_t n,
                       double previous_mid, double current_mid) {
    double prev_vol = 0.0, cur_vol = 0.0;
    const double prev_price = weighted_book_price(previous, previous + n, n,
                                                  &prev_vol, &cur_vol);
    // The helper above cannot distinguish sides when passed a contiguous pair;
    // this overload is implemented explicitly below by the caller.
    (void)previous; (void)current; (void)prev_price; (void)prev_vol;
    (void)cur_vol; (void)previous_mid; (void)current_mid;
    return 0.0;
}

double weighted_return_pair(const Level* pb, const Level* pa, const Level* cb,
                            const Level* ca, std::size_t n, double current_mid) {
    double pd = 0.0, pv = 0.0, cd = 0.0, cv = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (pb[i].quantity == 0 || pa[i].quantity == 0 ||
            cb[i].quantity == 0 || ca[i].quantity == 0) return 0.0;
        pd += price(pb[i]) * pb[i].quantity + price(pa[i]) * pa[i].quantity;
        pv += static_cast<double>(pb[i].quantity + pa[i].quantity);
        cd += price(cb[i]) * cb[i].quantity + price(ca[i]) * ca[i].quantity;
        cv += static_cast<double>(cb[i].quantity + ca[i].quantity);
    }
    if (pv == 0.0 || cv == 0.0 || current_mid <= 0.0) return 0.0;
    return ((cd / cv) - (pd / pv)) / current_mid * 1000.0;
}

}  // namespace

FactorValidity::FactorValidity()
    : has_two_sided_book(false), has_previous_book(false), has_flow(false),
      has_free_share(false), has_static_metadata(false),
      static_metadata_complete(false), complete(false) {}

FactorRow::FactorRow()
    : values(), validity(), mid_price(0.0), tick_index(0),
      factor_l1_ns(0), factor_flow_ns(0), factor_depth_build_ns(0),
      factor_depth_aggregate_ns(0), factor_finalize_ns(0), flow_event_count(0),
      live_order_count(0), bid_level_count(0), ask_level_count(0) {
    values.fill(0.0f);
}

const char* tick_factor_name(std::size_t index) {
    return index < kTickFactorCount ? kNames[index] : "";
}

FactorState::FactorState() : have_previous_(false), previous_(), free_share_(0.0),
                             have_free_share_(false), static_metadata_(),
                             have_static_metadata_(false), full_bids_(), full_asks_() {
    std::fill(previous_.bids, previous_.bids + 10, Level{0, 0, 0, 0, 0});
    std::fill(previous_.asks, previous_.asks + 10, Level{0, 0, 0, 0, 0});
}

void FactorState::reset() {
    have_previous_ = false;
    have_free_share_ = false;
    free_share_ = 0.0;
    static_metadata_ = DailyStaticMetadata();
    have_static_metadata_ = false;
    previous_ = BookPoint();
    full_bids_.clear();
    full_asks_.clear();
    std::fill(previous_.bids, previous_.bids + 10, Level{0, 0, 0, 0, 0});
    std::fill(previous_.asks, previous_.asks + 10, Level{0, 0, 0, 0, 0});
}

void FactorState::set_free_share(double value) {
    have_free_share_ = value > 0.0 && std::isfinite(value);
    free_share_ = have_free_share_ ? value : 0.0;
}

void FactorState::set_static_metadata(const DailyStaticMetadata& metadata) {
    static_metadata_ = metadata;
    have_static_metadata_ = true;
    set_free_share(metadata.has_free_share ? metadata.free_share : 0.0);
}

void FactorState::seed_window(double mid_price, std::uint64_t now_micros,
                              double volume, double turnover) {
    previous_ = BookPoint();
    std::fill(previous_.bids, previous_.bids + 10, Level{0, 0, 0, 0, 0});
    std::fill(previous_.asks, previous_.asks + 10, Level{0, 0, 0, 0, 0});
    previous_.mid = mid_price;
    previous_.volume = volume;
    previous_.turnover = turnover;
    previous_.time_micros = now_micros;
    previous_.two_sided = false;
    have_previous_ = mid_price > 0.0 && std::isfinite(mid_price);
}

const char* FactorState::static_quality() const {
    if (!have_static_metadata_) return "static_metadata_missing";
    if (!static_metadata_.complete()) return "static_metadata_incomplete";
    return "ok";
}

FactorRow FactorState::build(OrderBook& book, std::uint64_t now_micros,
                             double snapshot_last_price,
                             double snapshot_volume,
                             double snapshot_turnover) {
    FactorRow row;
    const std::uint64_t l1_started = factor_clock_ns();
    Level bids[10] = {}, asks[10] = {};
    book.snapshot(bids, asks, 10U);
    for (std::size_t i = 0; i < 10; ++i) {
        row.values[0] += 0.0f;  // keeps compilers from treating zero init as dead state
    }
    const bool two_sided = bids[0].quantity > 0 && asks[0].quantity > 0 &&
                           bids[0].price_raw > 0 && asks[0].price_raw > 0;
    const double bp = price(bids[0]);
    const double ap = price(asks[0]);
    double fallback_last = snapshot_last_price > 0.0 ? snapshot_last_price
                                                     : book.last_trade_price();
    if (fallback_last <= 0.0 && have_static_metadata_ &&
        static_metadata_.has_pre_close && static_metadata_.pre_close > 0.0)
        fallback_last = static_metadata_.pre_close;
    const double mid = mid_price(&bids[0], &asks[0], fallback_last);
    const double spread = two_sided ? ap - bp : 0.0;
    const double bq = static_cast<double>(bids[0].quantity);
    const double aq = static_cast<double>(asks[0].quantity);
    const FlowStats flow = book.take_flow_window();
    row.mid_price = mid;
    row.tick_index = book.last_tick_index();
    row.validity.has_two_sided_book = two_sided;
    row.validity.has_previous_book = have_previous_;
    row.validity.has_flow = !flow.events.empty();
    row.validity.has_free_share = have_free_share_;
    row.validity.has_static_metadata = have_static_metadata_;
    row.validity.static_metadata_complete = static_metadata_complete();
    row.live_order_count = static_cast<std::uint64_t>(book.live_order_count());

    const double hermes = classic_hermes(bids, asks, 5U, mid);
    row.values[0] = mid > 0.0 ? static_cast<float>((hermes / mid - 1.0) * 1000.0) : 0.0f;
    row.values[2] = static_cast<float>(safe_div(spread, mid) * 1000.0);
    double bid5 = 0.0, ask5 = 0.0;
    (void)weighted_book_price(bids, asks, 5U, &bid5, &ask5);
    const double bid_dot = [&]() { double v = 0.0; for (std::size_t i=0;i<5;++i) v += price(bids[i])*bids[i].quantity; return v; }();
    const double ask_dot = [&]() { double v = 0.0; for (std::size_t i=0;i<5;++i) v += price(asks[i])*asks[i].quantity; return v; }();
    const double current_ask_distance = safe_div(ask_dot, ask5) - mid;
    const double current_bid_distance = mid - safe_div(bid_dot, bid5);
    row.values[12] = static_cast<float>(safe_div(current_ask_distance, mid) * 1000.0);
    row.values[13] = static_cast<float>(safe_div(current_bid_distance, mid) * 1000.0);
    double bid_weighted = 0.0, ask_weighted = 0.0;
    for (std::size_t i = 0; i < 5U; ++i) {
        const double weight = static_cast<double>(5U - i);
        bid_weighted += bids[i].quantity * weight;
        ask_weighted += asks[i].quantity * weight;
    }
    row.values[16] = static_cast<float>(safe_div(ask_weighted, ask_weighted + bid_weighted) - 0.5);
    row.values[17] = static_cast<float>(safe_div(ask5, ask5 + bid5) - 0.5);
    row.values[19] = static_cast<float>(safe_div(aq, ask5));
    row.values[20] = static_cast<float>(safe_div(bq, bid5));

    if (have_previous_) {
        row.values[3] = static_cast<float>(safe_div(mid - previous_.mid, mid) * 1000.0);
        const double front = bq + aq;
        const double bid_delta = bp - price(previous_.bids[0]);
        const double ask_delta = ap - price(previous_.asks[0]);
        const double bid_change = front == 0.0 ? 0.0 :
            (bid_delta < -1e-6 ? -static_cast<double>(previous_.bids[0].quantity) / front :
             (bid_delta > 1e-6 ? (static_cast<double>(previous_.asks[0].quantity) + bq) / front :
              (bq - static_cast<double>(previous_.bids[0].quantity)) / front));
        const double ask_change = front == 0.0 ? 0.0 :
            (ask_delta < -1e-6 ? (static_cast<double>(previous_.bids[0].quantity) + aq) / front :
             (ask_delta > 1e-6 ? -static_cast<double>(previous_.asks[0].quantity) / front :
              (aq - static_cast<double>(previous_.asks[0].quantity)) / front));
        // Match the reference snapshot implementation's symmetric +/-200
        // clamp. Without this, thin-book transitions create impossible
        // values (e.g. 493 or 621) and dominate error metrics.
        row.values[5] = static_cast<float>(std::max(-200.0, std::min(200.0, bid_change)));
        row.values[6] = static_cast<float>(std::max(-200.0, std::min(200.0, ask_change)));
        for (std::size_t n = 1; n <= 5; ++n)
            row.values[7 + n - 1] = static_cast<float>(weighted_return_pair(
                previous_.bids, previous_.asks, bids, asks, n, mid));
        const double previous_ask_distance = weighted_price(previous_.asks, 5U, true,
                                                            previous_.mid);
        const double previous_bid_distance = weighted_price(previous_.bids, 5U, false,
                                                            previous_.mid);
        row.values[14] = static_cast<float>(safe_div(
            current_ask_distance - previous_ask_distance,
            previous_.mid) * 1000.0);
        row.values[15] = static_cast<float>(safe_div(
            current_bid_distance - previous_bid_distance,
            previous_.mid) * 1000.0);
    }
    row.values[4] = static_cast<float>(std::sqrt(std::max(0.0, 0.15 *
        (book.last_trade_price() > 0.0 ? book.last_trade_price() : mid))));
    double top5_turnover_base = 0.0;
    for (std::size_t i = 0; i < 5U; ++i)
        // Order-book quantities are canonical shares (converted once in
        // OrderBook::apply), so price*quantity is already model yuan and
        // matches current_turnover's currency unit.
        top5_turnover_base += price(bids[i]) * static_cast<double>(bids[i].quantity) +
                              price(asks[i]) * static_cast<double>(asks[i].quantity);
    const double current_turnover = snapshot_turnover >= 0.0 ? snapshot_turnover
                                                              : book.total_trade_turnover();
    const double previous_turnover = have_previous_ ? previous_.turnover : current_turnover;
    row.values[18] = static_cast<float>(safe_div(
        current_turnover - previous_turnover, top5_turnover_base));

    row.factor_l1_ns = factor_clock_ns() - l1_started;

    // Reconstruct the window flow using the start-of-window L1 prices, as in
    // the C++/C# reference implementation. SSE T carries both order ids and
    // therefore contributes to both fill and order-flow denominators.
    double positive_order = 0.0, negative_order = 0.0, market_flow = 0.0;
    double cancel_buy = 0.0, cancel_sell = 0.0, positive_trade = 0.0, negative_trade = 0.0;
    double buy_order = 0.0, sell_order = 0.0, buy_filled = 0.0, sell_filled = 0.0;
    const double start_bid = have_previous_ ? price(previous_.bids[0]) : bp;
    const double start_ask = have_previous_ ? price(previous_.asks[0]) : ap;
    const bool start_bid_present = have_previous_ && previous_.bids[0].quantity > 0;
    const bool start_ask_present = have_previous_ && previous_.asks[0].quantity > 0;

    if (have_previous_ && previous_.two_sided && two_sided) {
        const double current_volume = snapshot_volume >= 0.0 ? snapshot_volume
                                                              : static_cast<double>(book.total_trade_qty());
        // All cumulative volume/turnover inputs are canonical shares/yuan;
        // the window delta needs no further scaling.
        const double volume_delta = current_volume - previous_.volume;
        const double free_volume = have_free_share_ ? volume_delta / free_share_ : 0.0;
        const double prior_spread = price(previous_.asks[0]) - price(previous_.bids[0]);
        if (volume_delta > 0.0 && prior_spread > 0.0) {
            const double atp = (current_turnover - previous_.turnover) /
                               volume_delta;
            double r = (atp - previous_.mid) / prior_spread;
            r = std::max(-0.5, std::min(0.5, r));
            row.values[1] = static_cast<float>(
                std::sqrt(std::max(0.0, free_volume * (0.5 + r))) -
                std::sqrt(std::max(0.0, free_volume * (0.5 - r))));
        }
    }
    const std::uint64_t flow_started = factor_clock_ns();
    row.flow_event_count = static_cast<std::uint64_t>(flow.events.size());
    for (std::size_t i = 0; i < flow.events.size(); ++i) {
        const FlowEvent& event = flow.events[i];
        // EFH tick quantity was converted to canonical shares when the event
        // was applied to the book, so no further /1000 scaling is allowed.
        const double q = static_cast<double>(event.quantity);
        const double p = price_yuan(event.price_raw);
        if (event.kind == 'A') {
            if (event.side == 'B') {
                buy_order += q;
                if (start_bid_present) {
                    const double limit = start_ask_present ? start_ask : start_bid + 0.01;
                    if (p < limit - 1e-6 && p > 0.0 && start_bid > 0.0)
                        positive_order += q * (1.0 - std::tanh((start_bid / p - 1.0) * 100.0));
                }
                if (p > start_ask + 1e-6) market_flow += q;
            } else if (event.side == 'S') {
                sell_order += q;
                if (start_ask_present) {
                    const double limit = start_bid_present ? start_bid : start_ask - 0.01;
                    if (p > limit + 1e-6 && p > 0.0 && start_ask > 0.0)
                        negative_order += q * (1.0 - std::tanh((p / start_ask - 1.0) * 100.0));
                }
                if (p < start_bid - 1e-6) market_flow -= q;
            }
        } else if (event.kind == 'D') {
            if (event.side == 'B') cancel_buy += q; else if (event.side == 'S') cancel_sell += q;
        } else if (event.kind == 'T') {
            if (event.order_no > event.other_order_no) {
                positive_trade += q; buy_filled += q; buy_order += q;
                if (start_bid_present && p > 0.0 && start_bid > 0.0)
                    positive_order += q * (1.0 - std::tanh((start_bid / p - 1.0) * 100.0));
            } else if (event.order_no < event.other_order_no) {
                negative_trade += q; sell_filled += q; sell_order += q;
                if (start_ask_present && p > 0.0 && start_ask > 0.0)
                    negative_order += q * (1.0 - std::tanh((p / start_ask - 1.0) * 100.0));
            }
        }
    }
    if (have_free_share_) {
        const double bench = free_share_;
        row.values[21] = static_cast<float>(positive_order / bench);
        row.values[22] = static_cast<float>(negative_order / bench);
        row.values[23] = static_cast<float>(market_flow / bench);
        row.values[24] = static_cast<float>(cancel_buy / bench);
        row.values[25] = static_cast<float>(cancel_sell / bench);
        row.values[26] = static_cast<float>(positive_trade / bench);
        row.values[27] = static_cast<float>(negative_trade / bench);
        row.values[28] = static_cast<float>(std::min(1.0, safe_div(buy_filled, buy_order)));
        row.values[29] = static_cast<float>(std::min(1.0, safe_div(sell_filled, sell_order)));
        row.values[30] = static_cast<float>(safe_div(buy_order - sell_order, buy_order + sell_order + 1.0));
        const double buy_cfr = safe_div(buy_filled, buy_filled + cancel_buy + 1.0);
        const double sell_cfr = safe_div(sell_filled, sell_filled + cancel_sell + 1.0);
        row.values[31] = static_cast<float>(safe_div(buy_cfr - sell_cfr, buy_cfr + sell_cfr + 1.0));
    }
    row.factor_flow_ns = factor_clock_ns() - flow_started;

    const std::uint64_t depth_build_started = factor_clock_ns();
    book.full_depth('B', now_micros, &full_bids_);
    book.full_depth('S', now_micros, &full_asks_);
    row.factor_depth_build_ns = factor_clock_ns() - depth_build_started;
    row.bid_level_count = static_cast<std::uint64_t>(full_bids_.size());
    row.ask_level_count = static_cast<std::uint64_t>(full_asks_.size());
    const bool full_valid = two_sided && !full_bids_.empty() && !full_asks_.empty();
    if (full_valid) {
        const std::uint64_t depth_aggregate_started = factor_clock_ns();
        const double full_mid = (price(full_bids_[0]) + price(full_asks_[0])) * 0.5;
        const SideBands bid = aggregate_side(full_bids_, false, full_mid);
        const SideBands ask = aggregate_side(full_asks_, true, full_mid);
        const Band& bid01 = bid.dist01;
        const Band& ask01 = ask.dist01;
        const Band& bid05 = bid.dist05;
        const Band& ask05 = ask.dist05;
        const Band& bid10 = bid.dist10;
        const Band& ask10 = ask.dist10;
        const Band& bid1 = bid.top1;
        const Band& ask1 = ask.top1;
        const Band& bid5 = bid.top5;
        const Band& ask5b = ask.top5;
        row.values[32] = static_cast<float>(fix_imbalance(ask01, bid01));
        row.values[33] = static_cast<float>(fix_imbalance(ask05, bid05));
        row.values[34] = static_cast<float>(weighted_fix_imbalance(ask01, bid01, full_mid, 0.01));
        row.values[35] = static_cast<float>(weighted_fix_imbalance(ask05, bid05, full_mid, 0.05));
        row.values[36] = static_cast<float>(avg_size_imbalance(ask10, bid10));
        row.values[37] = static_cast<float>(avg_size_imbalance(ask1, bid1));
        row.values[38] = static_cast<float>(avg_size_imbalance(ask5b, bid5));
        row.values[39] = static_cast<float>(count_imbalance(ask10, bid10));
        row.values[40] = static_cast<float>(count_imbalance(ask1, bid1));
        row.values[41] = static_cast<float>(count_imbalance(ask5b, bid5));
        row.values[42] = static_cast<float>(life_imbalance(ask10, bid10, now_micros));
        row.values[43] = static_cast<float>(life_imbalance(ask1, bid1, now_micros));
        row.values[44] = static_cast<float>(life_imbalance(ask5b, bid5, now_micros));
        row.values[45] = static_cast<float>(safe_div(bid.max_price, full_mid) - 1.0);
        row.values[46] = static_cast<float>(safe_div(ask.max_price, full_mid) - 1.0);
        row.values[47] = static_cast<float>(max_vol_distance_imbalance(
            ask.max_price, bid.max_price, full_mid));
        row.values[48] = static_cast<float>(safe_div(ask.young_weighted -
            bid.young_weighted, ask.young_weighted + bid.young_weighted));
        const double effective_bid = bid.fix_weight > 0.0 ? bid.fix_dot / bid.fix_weight
                                                           : price(full_bids_[0]);
        const double effective_ask = ask.fix_weight > 0.0 ? ask.fix_dot / ask.fix_weight
                                                           : price(full_asks_[0]);
        const double hermes = (effective_bid + effective_ask) * 0.5;
        row.values[49] = static_cast<float>(hermes > 0.0
            ? std::max(-5.0, std::min(5.0, (hermes / full_mid - 1.0) * 1000.0)) : 0.0);
        row.factor_depth_aggregate_ns = factor_clock_ns() - depth_aggregate_started;
    }

    const std::uint64_t finalize_started = factor_clock_ns();
    for (std::size_t i = 0; i < kTickFactorCount; ++i)
        row.values[i] = static_cast<float>(finite_or_zero(row.values[i]));

    row.validity.complete = have_previous_ && previous_.two_sided && two_sided &&
                            full_valid && have_free_share_ &&
                            row.validity.static_metadata_complete;
    BookPoint current;
    current.mid = mid; current.spread = spread; current.bid_qty = bq; current.ask_qty = aq;
    current.volume = snapshot_volume >= 0.0 ? snapshot_volume : static_cast<double>(book.total_trade_qty());
    current.turnover = current_turnover; current.tick = book.last_tick_index();
    current.time_micros = now_micros; current.two_sided = two_sided;
    for (std::size_t i=0;i<10;++i) { current.bids[i]=bids[i]; current.asks[i]=asks[i]; }
    previous_ = current;
    have_previous_ = true;
    row.factor_finalize_ns = factor_clock_ns() - finalize_started;
    return row;
}

}  // namespace sse_tick
