#include "sse/factors/snapshot36.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace sse_snapshot36 {
namespace {

const std::size_t kLevels = 5U;
const double kPriceEpsilon = 1e-6;

double clamp_dotnet(double value, double minimum, double maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

double mid(const sse_live::Snapshot& value) {
    return (value.bid_prices[0] + value.ask_prices[0]) * 0.5;
}

double total(const std::int64_t* values) {
    double result = 0.0;
    for (std::size_t i = 0; i < kLevels; ++i) result += static_cast<double>(values[i]);
    return result;
}

double price_volume_dot(const double* prices, const std::int64_t* volumes) {
    double result = 0.0;
    for (std::size_t i = 0; i < kLevels; ++i) result += prices[i] * static_cast<double>(volumes[i]);
    return result;
}

double classic_hermes(const sse_live::Snapshot& value) {
    if (value.bid_volumes[0] == 0 || value.ask_volumes[0] == 0) return value.last_price;
    const double weights[kLevels] = {5.0, 4.0, 3.0, 2.0, 1.0};
    double sum = 0.0;
    double weight_sum = 0.0;
    for (std::size_t level = 0; level < kLevels; ++level) {
        const double bid = static_cast<double>(value.bid_volumes[level]);
        const double ask = static_cast<double>(value.ask_volumes[level]);
        if (bid == 0.0 || ask == 0.0) break;
        sum += (value.ask_prices[level] * bid + value.bid_prices[level] * ask) /
               (ask + bid) * weights[level];
        weight_sum += weights[level];
    }
    return sum / weight_sum;
}

double percent_hermes(const sse_live::Snapshot& value) {
    const double midpoint = mid(value);
    if (midpoint < 0.01 || midpoint > 1e6) return 0.0;
    return clamp_dotnet((classic_hermes(value) / midpoint - 1.0) * 1e3, -5.0, 5.0);
}

double percent_atp_reverse(const sse_live::Snapshot& start,
                           const sse_live::Snapshot& current) {
    if (current.volume == start.volume) return 0.0;
    const double atp = (current.turnover - start.turnover) /
                       static_cast<double>(current.volume - start.volume);
    return atp / mid(start) - 1.0;
}

double ask_volume_change_ratio(const sse_live::Snapshot& start,
                               const sse_live::Snapshot& current) {
    const double front = static_cast<double>(current.ask_volumes[0] + current.bid_volumes[0]);
    if (front == 0.0) return 0.0;
    double value = 0.0;
    if (current.ask_prices[0] - start.ask_prices[0] < -kPriceEpsilon)
        value = static_cast<double>(start.bid_volumes[0] + current.ask_volumes[0]) / front;
    else if (current.ask_prices[0] - start.ask_prices[0] > kPriceEpsilon)
        value = -static_cast<double>(start.ask_volumes[0]) / front;
    else
        value = static_cast<double>(current.ask_volumes[0] - start.ask_volumes[0]) / front;
    return clamp_dotnet(value, -200.0, 200.0);
}

double bid_volume_change_ratio(const sse_live::Snapshot& start,
                               const sse_live::Snapshot& current) {
    const double front = static_cast<double>(current.ask_volumes[0] + current.bid_volumes[0]);
    if (front == 0.0) return 0.0;
    double value = 0.0;
    if (current.bid_prices[0] - start.bid_prices[0] < -kPriceEpsilon)
        value = -static_cast<double>(start.bid_volumes[0]) / front;
    else if (current.bid_prices[0] - start.bid_prices[0] > kPriceEpsilon)
        value = static_cast<double>(start.ask_volumes[0] + current.bid_volumes[0]) / front;
    else
        value = static_cast<double>(current.bid_volumes[0] - start.bid_volumes[0]) / front;
    return clamp_dotnet(value, -200.0, 200.0);
}

double sqrt_positive(const sse_live::Snapshot& start, const sse_live::Snapshot& current) {
    const std::int64_t volume = current.volume - start.volume;
    if (volume == 0) return 0.0;
    const double atp = (current.turnover - start.turnover) / static_cast<double>(volume);
    const double spread = start.ask_prices[0] - start.bid_prices[0];
    const double ratio = clamp_dotnet((atp - mid(start)) / spread, -0.5, 0.5);
    return std::sqrt(static_cast<double>(volume) * (0.5 + ratio)) -
           std::sqrt(static_cast<double>(volume) * (0.5 - ratio));
}

double price_pressure(const sse_live::Snapshot& current, std::size_t level) {
    const std::int64_t ask = current.ask_volumes[level];
    const std::int64_t bid = current.bid_volumes[level];
    if (ask == 0 || bid == 0) return 0.0;
    const double count = static_cast<double>(ask + bid);
    return current.ask_prices[level] * static_cast<double>(bid) / count +
           current.bid_prices[level] * static_cast<double>(ask) / count - mid(current);
}

double volume_pressure(const sse_live::Snapshot& current, std::size_t level) {
    if (current.ask_volumes[level] == 0 || current.bid_volumes[level] == 0) return 0.0;
    const double ask = std::sqrt(static_cast<double>(current.ask_volumes[level]));
    const double bid = std::sqrt(static_cast<double>(current.bid_volumes[level]));
    return (ask - bid) / (ask + bid);
}

double weighted_ask(const sse_live::Snapshot& value) {
    const double volume = total(value.ask_volumes);
    return volume == 0.0 ? 0.0 : price_volume_dot(value.ask_prices, value.ask_volumes) / volume - mid(value);
}

double weighted_bid(const sse_live::Snapshot& value) {
    const double volume = total(value.bid_volumes);
    return volume == 0.0 ? 0.0 : mid(value) - price_volume_dot(value.bid_prices, value.bid_volumes) / volume;
}

double weighted_skew(const sse_live::Snapshot& start, const sse_live::Snapshot& current,
                     bool ask) {
    const std::int64_t* start_volume = ask ? start.ask_volumes : start.bid_volumes;
    const std::int64_t* current_volume = ask ? current.ask_volumes : current.bid_volumes;
    const double* start_price = ask ? start.ask_prices : start.bid_prices;
    const double* current_price = ask ? current.ask_prices : current.bid_prices;
    const double first = total(start_volume);
    const double second = total(current_volume);
    if (first * second == 0.0) return 0.0;
    return price_volume_dot(current_price, current_volume) / second -
           price_volume_dot(start_price, start_volume) / first;
}

bool contains_zero(const std::int64_t* values) {
    for (std::size_t i = 0; i < kLevels; ++i) if (values[i] == 0) return true;
    return false;
}

double order_move(const sse_live::Snapshot& start, const sse_live::Snapshot& current,
                  bool ask) {
    const std::int64_t* sv = ask ? start.ask_volumes : start.bid_volumes;
    const std::int64_t* cv = ask ? current.ask_volumes : current.bid_volumes;
    const double* sp = ask ? start.ask_prices : start.bid_prices;
    const double* cp = ask ? current.ask_prices : current.bid_prices;
    if (contains_zero(sv) || contains_zero(cv)) return 0.0;
    double weighted = 0.0;
    double volume = 0.0;
    if ((ask && cp[0] > sp[0]) || (!ask && cp[0] < sp[0])) {
        for (std::size_t level = 0; level < kLevels; ++level) {
            if ((ask && sp[level] >= cp[0]) || (!ask && sp[level] <= cp[0])) break;
            weighted += sp[level] * static_cast<double>(sv[level]);
            volume += static_cast<double>(sv[level]);
        }
    } else if ((ask && cp[0] < sp[0]) || (!ask && cp[0] > sp[0])) {
        for (std::size_t level = 0; level < kLevels; ++level) {
            if ((ask && cp[level] >= sp[0]) || (!ask && cp[level] <= sp[0])) break;
            weighted += cp[level] * static_cast<double>(cv[level]);
            volume += static_cast<double>(cv[level]);
        }
    }
    return volume == 0.0 ? 0.0 : weighted / volume - sp[0];
}

double weighted_price(const sse_live::Snapshot& value, std::size_t levels) {
    double volume = 0.0;
    double notional = 0.0;
    for (std::size_t level = 0; level < levels; ++level) {
        volume += static_cast<double>(value.ask_volumes[level] + value.bid_volumes[level]);
        notional += value.ask_prices[level] * static_cast<double>(value.ask_volumes[level]) +
                    value.bid_prices[level] * static_cast<double>(value.bid_volumes[level]);
    }
    return volume == 0.0 ? mid(value) : notional / volume;
}

double weighted_volume_difference(const sse_live::Snapshot& value) {
    double ask = 0.0, bid = 0.0;
    for (std::size_t level = 0; level < kLevels; ++level) {
        const double weight = static_cast<double>(kLevels - level);
        ask += static_cast<double>(value.ask_volumes[level]) * weight;
        bid += static_cast<double>(value.bid_volumes[level]) * weight;
    }
    return ask + bid == 0.0 ? 0.0 : ask / (ask + bid) - 0.5;
}

double volume_difference(const sse_live::Snapshot& value) {
    const double ask = total(value.ask_volumes), bid = total(value.bid_volumes);
    return ask + bid == 0.0 ? 0.0 : ask / (ask + bid) - 0.5;
}

double volume_shift(const sse_live::Snapshot& start, const sse_live::Snapshot& current,
                    std::size_t level, bool ask) {
    const std::int64_t* sv = ask ? start.ask_volumes : start.bid_volumes;
    const std::int64_t* cv = ask ? current.ask_volumes : current.bid_volumes;
    const double* sp = ask ? start.ask_prices : start.bid_prices;
    const double* cp = ask ? current.ask_prices : current.bid_prices;
    if (sv[level] == 0) return 0.0;
    std::int64_t difference = 0;
    if (std::fabs(cp[level] - sp[level]) < kPriceEpsilon) {
        difference = cv[level] - sv[level];
    } else if ((ask && cp[level] < sp[level] - kPriceEpsilon) ||
               (!ask && cp[level] > sp[level] + kPriceEpsilon)) {
        difference = cv[level];
        for (std::size_t deeper = level + 1U; deeper < kLevels; ++deeper) {
            if ((ask && cp[deeper] < sp[level] - kPriceEpsilon) ||
                (!ask && cp[deeper] > sp[level] + kPriceEpsilon)) {
                difference += cv[deeper];
                continue;
            }
            if (std::fabs(cp[deeper] - sp[level]) < kPriceEpsilon) difference += cv[deeper] - sv[level];
            break;
        }
    } else {
        difference = -cv[level];
        for (std::size_t shallower = level; shallower-- > 0U;) {
            if ((ask && cp[shallower] > sp[level] + kPriceEpsilon) ||
                (!ask && cp[shallower] < sp[level] - kPriceEpsilon)) {
                difference -= cv[shallower];
                continue;
            }
            if (std::fabs(cp[shallower] - sp[level]) < kPriceEpsilon) difference -= cv[shallower] - sv[level];
            break;
        }
    }
    return clamp_dotnet(static_cast<double>(difference) / static_cast<double>(sv[level]), -5.0, 5.0);
}

}  // namespace

bool valid(const sse_live::Snapshot& value) {
    return value.ask_prices[0] > 0.0 && std::isfinite(value.ask_prices[0]) &&
           value.bid_prices[0] > 0.0 && std::isfinite(value.bid_prices[0]) &&
           value.ask_volumes[0] > 0 && value.bid_volumes[0] > 0 &&
           value.volume >= 0 && value.turnover >= 0.0 && std::isfinite(value.turnover);
}

std::vector<float> build(const sse_live::Snapshot& start, const sse_live::Snapshot& current) {
    const double midpoint = mid(current);
    const double start_mid = mid(start);
    const std::int64_t delta_volume = current.volume - start.volume;
    const double positive = delta_volume == 0 ? 0.0 :
        sqrt_positive(start, current) / std::sqrt(static_cast<double>(delta_volume));
    std::vector<double> values;
    values.reserve(36U);
    values.push_back(percent_hermes(current));
    values.push_back(percent_hermes(current) - percent_hermes(start));
    values.push_back((midpoint - start_mid) / midpoint * 1e3);
    values.push_back(clamp_dotnet(percent_atp_reverse(start, current) * 1e3, -10.0, 10.0));
    values.push_back(clamp_dotnet(ask_volume_change_ratio(start, current), -5.0, 5.0));
    values.push_back(clamp_dotnet(bid_volume_change_ratio(start, current), -5.0, 5.0));
    values.push_back(positive);
    values.push_back(price_pressure(current, 0) / midpoint * 1e3);
    values.push_back(price_pressure(current, 1) / midpoint * 1e3);
    values.push_back(volume_pressure(current, 0));
    values.push_back(volume_pressure(current, 1));
    values.push_back(weighted_skew(start, current, true) / midpoint * 1e3);
    values.push_back(weighted_skew(start, current, false) / midpoint * 1e3);
    values.push_back(order_move(start, current, true) / midpoint * 1e3);
    values.push_back(order_move(start, current, false) / midpoint * 1e3);
    values.push_back((current.ask_prices[0] - current.bid_prices[0]) / midpoint * 1e3);
    for (std::size_t level = 1U; level <= kLevels; ++level)
        values.push_back(clamp_dotnet((weighted_price(current, level) - weighted_price(start, level)) /
                                      midpoint * 1e3, -100.0, 100.0));
    values.push_back(weighted_ask(current) / midpoint * 1e3);
    values.push_back(weighted_bid(current) / midpoint * 1e3);
    values.push_back((weighted_ask(current) - weighted_ask(start)) / start_mid * 1e3);
    values.push_back((weighted_bid(current) - weighted_bid(start)) / start_mid * 1e3);
    values.push_back(weighted_volume_difference(current));
    values.push_back(volume_difference(current));
    const double front = price_volume_dot(current.ask_prices, current.ask_volumes) +
                         price_volume_dot(current.bid_prices, current.bid_volumes);
    values.push_back(front == 0.0 ? 0.0 : (current.turnover - start.turnover) / front);
    values.push_back(static_cast<double>(current.ask_volumes[0]) / total(current.ask_volumes));
    values.push_back(static_cast<double>(current.bid_volumes[0]) / total(current.bid_volumes));
    for (std::size_t level = 0; level < 3U; ++level) values.push_back(volume_shift(start, current, level, true));
    for (std::size_t level = 0; level < 3U; ++level) values.push_back(volume_shift(start, current, level, false));
    std::vector<float> result(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) result[i] = static_cast<float>(values[i]);
    return result;
}

}  // namespace sse_snapshot36
