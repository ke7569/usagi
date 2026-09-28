#include "sze/sampling/mix153060_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <ctime>
#include <functional>
#include <limits>
#include <map>
#include <time.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mix153060 {

namespace {

const int64_t kDayUs = 86400000000LL;
const int64_t kMorningOpenUs = 34200000000LL;
const int64_t kMorningCloseUs = 41400000000LL;
const int64_t kAfternoonOpenUs = 46800000000LL;
const int64_t kFeatureEndUs = 53820000000LL;
const int64_t kChangeStartUs = 34260000000LL;
const int64_t kCloseUs = 54000000000LL;
const int64_t kTimeTriggerUs = 100000000LL;
const int64_t kChangeMinVolume = 100;
const int64_t kMaxChangeLocalGapUs = 10000;
const double kYoungAgeSeconds = 30.0;
const double kEpsilon = 1.0e-12;

int64_t positive_mod(int64_t value, int64_t modulus) {
    int64_t result = value % modulus;
    return result < 0 ? result + modulus : result;
}

int64_t time_of_day(int64_t value) {
    return positive_mod(value, kDayUs);
}

int session_id(int64_t value, bool v06_baseline = false) {
    const int64_t tod = time_of_day(value);
    if ((v06_baseline ? tod > kMorningOpenUs : tod >= kMorningOpenUs) &&
        tod <= kMorningCloseUs) {
        return 1;
    }
    const int64_t end_us = v06_baseline ? 53760000000LL : kFeatureEndUs;
    if (tod >= kAfternoonOpenUs && tod < end_us) {
        return 2;
    }
    return 0;
}

bool same_session(int64_t left, int64_t right, bool v06_baseline = false) {
    const int left_session = session_id(left, v06_baseline);
    return left_session != 0 && left_session == session_id(right, v06_baseline);
}

bool valid_trading_date(int32_t date) {
    const int year = date / 10000;
    const int month = date / 100 % 100;
    const int day = date % 100;
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    static const int days_by_month[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int days = days_by_month[month - 1];
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month == 2 && leap) {
        ++days;
    }
    return day <= days;
}

int price_tick(double price) {
    if (!std::isfinite(price) || price <= 0.0) {
        return 0;
    }
    const double scaled = std::floor(price * 100.0 + 0.5);
    if (scaled > static_cast<double>(std::numeric_limits<int>::max())) {
        return 0;
    }
    return static_cast<int>(scaled);
}

double tick_price(int tick) {
    return static_cast<double>(tick) * 0.01;
}

double safe_div(double numerator, double denominator) {
    return std::fabs(denominator) < kEpsilon ? 0.0 : numerator / denominator;
}

double per_mille(double delta, double reference) {
    return safe_div(delta, reference) * 1000.0;
}

double clamp(double value, double low, double high) {
    return std::min(std::max(value, low), high);
}

struct NativeOrder {
    int64_t id;
    bool buy;
    int tick;
    int64_t remaining;
    int64_t insert_us;
};

struct NativeLevel {
    int tick;
    int64_t volume;
    __int128 insert_sum_us;
    int64_t young_volume;
    std::unordered_map<int64_t, NativeOrder> orders;

    explicit NativeLevel(int value = 0)
        : tick(value), volume(0), insert_sum_us(0), young_volume(0), orders() {
        orders.reserve(32);
    }
};

struct Locator {
    bool buy;
    int tick;
};

struct LevelSnapshot {
    int tick;
    int64_t volume;
    int64_t count;
    int64_t age_sum_us;

    LevelSnapshot()
        : tick(0), volume(0), count(0), age_sum_us(0) {}
};

struct Cut {
    int64_t cut_index;
    int64_t app_sequence;
    int64_t exchange_time_us;
    int64_t local_time_us;
    double mid;
    int mid_source;
    int limit_state;
    double last_price;
    double turnover;
    int64_t volume;
    std::array<LevelSnapshot, kTopLevels> bids;
    std::array<LevelSnapshot, kTopLevels> asks;
    std::size_t bid_count;
    std::size_t ask_count;

    Cut()
        : cut_index(0),
          app_sequence(0),
          exchange_time_us(0),
          local_time_us(0),
          mid(0.0),
          mid_source(0),
          limit_state(0),
          last_price(0.0),
          turnover(0.0),
          volume(0),
          bids(),
          asks(),
          bid_count(0),
          ask_count(0) {}

    bool has_two_sided_l1() const {
        return bid_count > 0 && ask_count > 0 &&
               bids[0].volume > 0 && asks[0].volume > 0;
    }

    double best_bid_price() const {
        return bid_count == 0 ? 0.0 : tick_price(bids[0].tick);
    }

    double best_ask_price() const {
        return ask_count == 0 ? 0.0 : tick_price(asks[0].tick);
    }

    int64_t best_bid_volume() const {
        return bid_count == 0 ? 0 : bids[0].volume;
    }

    int64_t best_ask_volume() const {
        return ask_count == 0 ? 0 : asks[0].volume;
    }
};

class NativeBook {
public:
    NativeBook() : bids_(), asks_(), locators_(), young_entries_(), young_cutoff_us_(0) {}

    void clear() {
        bids_.clear();
        asks_.clear();
        locators_.clear();
        young_entries_.clear();
        young_cutoff_us_ = 0;
    }

    bool add(const OrderEvent& event) {
        int tick = price_tick(event.price);
        if (event.kind == OrderKind::kMarket) {
            if (event.buy && !asks_.empty()) {
                tick = asks_.begin()->first;
            } else if (!event.buy && !bids_.empty()) {
                tick = bids_.begin()->first;
            }
        } else if (event.kind == OrderKind::kSelfBest) {
            if (event.buy && !bids_.empty()) {
                tick = bids_.begin()->first;
            } else if (!event.buy && !asks_.empty()) {
                tick = asks_.begin()->first;
            }
        }
        if (event.app_sequence <= 0 || event.volume <= 0 || tick <= 0) {
            return false;
        }
        if (locators_.find(event.app_sequence) != locators_.end()) {
            return false;
        }
        advance_age(event.exchange_time_us);
        NativeOrder order;
        order.id = event.app_sequence;
        order.buy = event.buy;
        order.tick = tick;
        order.remaining = event.volume;
        order.insert_us = event.exchange_time_us;
        if (event.buy) {
            if (!add_to_side(&bids_, tick, order)) {
                return false;
            }
        } else {
            if (!add_to_side(&asks_, tick, order)) {
                return false;
            }
        }
        Locator locator;
        locator.buy = event.buy;
        locator.tick = tick;
        locators_[event.app_sequence] = locator;
        young_entries_.push_back(AgeEntry{order.id, order.insert_us, order.buy, order.tick});
        return true;
    }

    bool fill(const TradeEvent& event) {
        advance_age(event.exchange_time_us);
        if (event.kind == TradeKind::kCancel) {
            const int64_t id = std::max(event.buy_order_id, event.sell_order_id);
            return remove(id, -1);
        }
        if (event.buy_order_id <= 0 || event.sell_order_id <= 0 ||
            event.buy_order_id == event.sell_order_id ||
            !contains(event.buy_order_id) || !contains(event.sell_order_id)) {
            return false;
        }
        return remove(event.buy_order_id, event.volume) &&
               remove(event.sell_order_id, event.volume);
    }

    bool contains(int64_t id) const {
        return locators_.find(id) != locators_.end();
    }

    std::size_t level_count(bool buy) const {
        return buy ? static_cast<std::size_t>(bids_.size())
                   : static_cast<std::size_t>(asks_.size());
    }

    int64_t best_volume(bool buy) const {
        if (buy) {
            return bids_.empty() ? 0 : bids_.begin()->second.volume;
        }
        return asks_.empty() ? 0 : asks_.begin()->second.volume;
    }

    int best_tick(bool buy) const {
        if (buy) {
            return bids_.empty() ? 0 : bids_.begin()->first;
        }
        return asks_.empty() ? 0 : asks_.begin()->first;
    }

    template <typename Fn>
    void for_each_level(bool buy, Fn fn) const {
        if (buy) {
            for (BuyMap::const_iterator it = bids_.begin(); it != bids_.end(); ++it) {
                fn(it->second);
            }
        } else {
            for (SellMap::const_iterator it = asks_.begin(); it != asks_.end(); ++it) {
                fn(it->second);
            }
        }
    }

    std::size_t copy_top(bool buy,
                         std::array<LevelSnapshot, kTopLevels>* destination) const {
        if (destination == 0) {
            return 0;
        }
        std::size_t count = 0;
        if (buy) {
            for (BuyMap::const_iterator it = bids_.begin();
                 it != bids_.end() && count < kTopLevels; ++it, ++count) {
                copy_level(it->second, &(*destination)[count]);
            }
        } else {
            for (SellMap::const_iterator it = asks_.begin();
                 it != asks_.end() && count < kTopLevels; ++it, ++count) {
                copy_level(it->second, &(*destination)[count]);
            }
        }
        return count;
    }

private:
    struct AgeEntry {
        int64_t id;
        int64_t insert_us;
        bool buy;
        int tick;
    };
    typedef std::map<int, NativeLevel, std::greater<int> > BuyMap;
    typedef std::map<int, NativeLevel> SellMap;

    template <typename Map>
    static void expire(Map& side, const AgeEntry& entry) {
        const auto level = side.find(entry.tick);
        if (level == side.end()) return;
        const auto order = level->second.orders.find(entry.id);
        if (order == level->second.orders.end() || order->second.insert_us != entry.insert_us) return;
        level->second.young_volume -= order->second.remaining;
    }

    void advance_age(int64_t now_us) {
        // Runtime validates nondecreasing exchange timestamps before book mutation.
        // Exactly 30 seconds remains young; retain full microsecond precision.
        young_cutoff_us_ = now_us - 30000000LL;
        while (!young_entries_.empty() && young_entries_.front().insert_us < young_cutoff_us_) {
            const AgeEntry entry = young_entries_.front();
            if (entry.buy) expire(bids_, entry); else expire(asks_, entry);
            young_entries_.pop_front();
        }
    }

    template <typename Map>
    static bool add_to_side(Map* side, int tick, const NativeOrder& order) {
        NativeLevel& level = (*side)[tick];
        if (order.remaining > std::numeric_limits<int64_t>::max() - level.volume) {
            if (level.orders.empty()) {
                side->erase(tick);
            }
            return false;
        }
        level.tick = tick;
        level.orders[order.id] = order;
        level.volume += order.remaining;
        level.insert_sum_us += order.insert_us;
        level.young_volume += order.remaining;
        return true;
    }

    static void copy_level(const NativeLevel& level, LevelSnapshot* destination) {
        destination->tick = level.tick;
        destination->volume = level.volume;
        destination->count = static_cast<int64_t>(level.orders.size());
    }

    bool remove(int64_t id, int64_t quantity) {
        if (id == 0) {
            return false;
        }
        std::unordered_map<int64_t, Locator>::iterator locator_it = locators_.find(id);
        if (locator_it == locators_.end()) {
            return false;
        }
        const Locator locator = locator_it->second;
        if (locator.buy) {
            BuyMap::iterator level_it = bids_.find(locator.tick);
            if (level_it == bids_.end()) {
                locators_.erase(locator_it);
                return false;
            }
            return erase_from_level(&bids_, level_it, id, quantity, locator_it);
        } else {
            SellMap::iterator level_it = asks_.find(locator.tick);
            if (level_it == asks_.end()) {
                locators_.erase(locator_it);
                return false;
            }
            return erase_from_level(&asks_, level_it, id, quantity, locator_it);
        }
    }

    template <typename Map>
    bool erase_from_level(Map* side,
                          typename Map::iterator level_it,
                          int64_t id,
                          int64_t quantity,
                          std::unordered_map<int64_t, Locator>::iterator locator_it) {
        NativeLevel& level = level_it->second;
        typename std::unordered_map<int64_t, NativeOrder>::iterator order_it =
            level.orders.find(id);
        if (order_it == level.orders.end()) {
            locators_.erase(locator_it);
            return false;
        }
        NativeOrder& order = order_it->second;
        const int64_t removed = quantity < 0
                                    ? order.remaining
                                    : std::min(order.remaining, std::max<int64_t>(quantity, 0));
        order.remaining -= removed;
        level.volume -= removed;
        if (order.insert_us >= young_cutoff_us_) level.young_volume -= removed;
        if (order.remaining <= 0) {
            level.insert_sum_us -= order.insert_us;
            level.orders.erase(order_it);
            locators_.erase(locator_it);
        }
        if (level.orders.empty()) {
            side->erase(level_it);
        }
        return true;
    }

    BuyMap bids_;
    SellMap asks_;
    std::unordered_map<int64_t, Locator> locators_;
    std::deque<AgeEntry> young_entries_;
    int64_t young_cutoff_us_;
};

// Helpers that work with the two differently ordered side maps without
// exposing the book container in the public API.
struct SideStats {
    double volume;
    double count;
    int64_t age_sum_us;

    SideStats()
        : volume(0.0), count(0.0), age_sum_us(0) {}
};

struct Flow {
    double turnover;
    double positive_order_amount;
    double negative_order_amount;
    double market_buy_amount;
    double market_sell_amount;
    double cancel_buy_amount;
    double cancel_sell_amount;
    double positive_trade_amount;
    double negative_trade_amount;
    double buy_order_volume;
    double sell_order_volume;
    double buy_filled_volume;
    double sell_filled_volume;
    double buy_filled_amount;
    double sell_filled_amount;

    Flow() { clear(); }

    void clear() {
        turnover = 0.0;
        positive_order_amount = 0.0;
        negative_order_amount = 0.0;
        market_buy_amount = 0.0;
        market_sell_amount = 0.0;
        cancel_buy_amount = 0.0;
        cancel_sell_amount = 0.0;
        positive_trade_amount = 0.0;
        negative_trade_amount = 0.0;
        buy_order_volume = 0.0;
        sell_order_volume = 0.0;
        buy_filled_volume = 0.0;
        sell_filled_volume = 0.0;
        buy_filled_amount = 0.0;
        sell_filled_amount = 0.0;
    }

    static double amount(double price, double volume) {
        return std::isfinite(price) && price > 0.0 &&
                       std::isfinite(volume) && volume > 0.0
                   ? price * volume
                   : 0.0;
    }

    void on_order(const Cut& start, const OrderEvent& event, bool v06_baseline = false) {
        const double volume = static_cast<double>(std::max<int64_t>(event.volume, 0));
        const double raw_price = event.price;
        if (event.buy) {
            buy_order_volume += volume;
            if (event.kind != OrderKind::kLimit ||
                ((!v06_baseline || start.best_ask_price() > 0.0) &&
                 raw_price > start.best_ask_price() + 1.0e-6)) {
                market_buy_amount += volume;
            }
            if (v06_baseline || start.best_bid_volume() != 0) {
                const double bench = v06_baseline && start.best_bid_price() <= 0.0
                                         ? raw_price : start.best_bid_price();
                const double limit = start.best_ask_volume() != 0
                                         ? start.best_ask_price()
                                         : bench + 0.01;
                if (raw_price < limit - 1.0e-6 && raw_price > 0.0 && bench > 0.0) {
                    positive_order_amount += volume *
                        (1.0 - std::tanh((bench / raw_price - 1.0) * 100.0));
                }
            }
        } else {
            sell_order_volume += volume;
            if (event.kind != OrderKind::kLimit ||
                ((!v06_baseline || start.best_bid_price() > 0.0) &&
                 raw_price < start.best_bid_price() - 1.0e-6)) {
                market_sell_amount += volume;
            }
            if (v06_baseline || start.best_ask_volume() != 0) {
                const double bench = v06_baseline && start.best_ask_price() <= 0.0
                                         ? raw_price : start.best_ask_price();
                const double limit = start.best_bid_volume() != 0
                                         ? start.best_bid_price()
                                         : bench - 0.01;
                if (raw_price > limit + 1.0e-6 && raw_price > 0.0 && bench > 0.0) {
                    negative_order_amount += volume *
                        (1.0 - std::tanh((raw_price / bench - 1.0) * 100.0));
                }
            }
        }
    }

    void on_trade(const TradeEvent& event, double cancel_price) {
        const double volume = static_cast<double>(std::max<int64_t>(event.volume, 0));
        const int64_t buy_id = event.buy_order_id;
        const int64_t sell_id = event.sell_order_id;
        if (event.kind == TradeKind::kCancel) {
            if (sell_id == 0) {
                cancel_buy_amount += volume;
            } else if (buy_id == 0) {
                cancel_sell_amount += volume;
            }
            return;
        }
        const double value = amount(event.price, volume);
        turnover += value;
        if (buy_id > sell_id) {
            positive_trade_amount += volume;
            buy_filled_volume += volume;
            buy_filled_amount += value;
        } else if (buy_id < sell_id) {
            negative_trade_amount += volume;
            sell_filled_volume += volume;
            sell_filled_amount += value;
        }
    }
};

double imbalance(double ask, double bid) {
    return safe_div(ask - bid, ask + bid);
}

double level_price(const std::array<LevelSnapshot, kTopLevels>& levels,
                   std::size_t count,
                   std::size_t index) {
    return index < count ? tick_price(levels[index].tick) : 0.0;
}

int64_t level_volume(const std::array<LevelSnapshot, kTopLevels>& levels,
                     std::size_t count,
                     std::size_t index) {
    return index < count ? levels[index].volume : 0;
}

double classic_hermes(const Cut& cut) {
    if (cut.best_bid_volume() == 0 || cut.best_ask_volume() == 0) {
        return cut.last_price;
    }
    static const double weights[kModelDepth] = {5.0, 4.0, 3.0, 2.0, 1.0};
    double total = 0.0;
    double weight_sum = 0.0;
    for (std::size_t i = 0; i < kModelDepth; ++i) {
        const int64_t bid_volume = level_volume(cut.bids, cut.bid_count, i);
        const int64_t ask_volume = level_volume(cut.asks, cut.ask_count, i);
        if (bid_volume == 0 || ask_volume == 0) {
            break;
        }
        total += (level_price(cut.asks, cut.ask_count, i) * bid_volume +
                  level_price(cut.bids, cut.bid_count, i) * ask_volume) /
                 static_cast<double>(ask_volume + bid_volume) * weights[i];
        weight_sum += weights[i];
    }
    return safe_div(total, weight_sum);
}

double weighted_distance(const Cut& cut, bool buy) {
    const std::array<LevelSnapshot, kTopLevels>& levels = buy ? cut.bids : cut.asks;
    const std::size_t count = buy ? cut.bid_count : cut.ask_count;
    double dot = 0.0;
    double volume = 0.0;
    for (std::size_t i = 0; i < kModelDepth; ++i) {
        const double level_volume_value = static_cast<double>(level_volume(levels, count, i));
        dot += level_price(levels, count, i) * level_volume_value;
        volume += level_volume_value;
    }
    if (volume == 0.0) {
        return 0.0;
    }
    const double weighted = dot / volume;
    return buy ? cut.mid - weighted : weighted - cut.mid;
}

double volume_change(const Cut& start, const Cut& current, bool buy) {
    const int64_t current_bid = current.best_bid_volume();
    const int64_t current_ask = current.best_ask_volume();
    const double front = static_cast<double>(current_bid + current_ask);
    if (front == 0.0) {
        return 0.0;
    }
    double raw = 0.0;
    if (buy) {
        const double delta = current.best_bid_price() - start.best_bid_price();
        if (delta < -1.0e-6) {
            raw = -static_cast<double>(start.best_bid_volume()) / front;
        } else if (delta > 1.0e-6) {
            raw = (static_cast<double>(start.best_ask_volume()) + current_bid) / front;
        } else {
            raw = static_cast<double>(current_bid - start.best_bid_volume()) / front;
        }
    } else {
        const double delta = current.best_ask_price() - start.best_ask_price();
        if (delta < -1.0e-6) {
            raw = (static_cast<double>(start.best_bid_volume()) + current_ask) / front;
        } else if (delta > 1.0e-6) {
            raw = -static_cast<double>(start.best_ask_volume()) / front;
        } else {
            raw = static_cast<double>(current_ask - start.best_ask_volume()) / front;
        }
    }
    return clamp(raw, -200.0, 200.0);
}

void weighted_returns(const Cut& start,
                      const Cut& current,
                      std::array<double, kModelDepth>* result) {
    result->fill(0.0);
    if (std::abs(start.limit_state) == 1 || std::abs(current.limit_state) == 1) {
        return;
    }
    double start_dot = 0.0;
    double current_dot = 0.0;
    double start_bid = 0.0;
    double start_ask = 0.0;
    double current_bid = 0.0;
    double current_ask = 0.0;
    for (std::size_t i = 0; i < kModelDepth; ++i) {
        const double sb = static_cast<double>(level_volume(start.bids, start.bid_count, i));
        const double sa = static_cast<double>(level_volume(start.asks, start.ask_count, i));
        const double cb = static_cast<double>(level_volume(current.bids, current.bid_count, i));
        const double ca = static_cast<double>(level_volume(current.asks, current.ask_count, i));
        start_dot += level_price(start.bids, start.bid_count, i) * sb +
                     level_price(start.asks, start.ask_count, i) * sa;
        current_dot += level_price(current.bids, current.bid_count, i) * cb +
                       level_price(current.asks, current.ask_count, i) * ca;
        start_bid += sb;
        start_ask += sa;
        current_bid += cb;
        current_ask += ca;
        if (start_bid > 0.0 && start_ask > 0.0 && current_bid > 0.0 && current_ask > 0.0) {
            const double start_weighted = start_dot / (start_bid + start_ask);
            const double current_weighted = current_dot / (current_bid + current_ask);
            (*result)[i] = clamp(per_mille(current_weighted - start_weighted, current.mid),
                                 -100.0, 100.0);
        }
    }
}

}  // namespace

StaticInputs::StaticInputs()
    : instrument(),
      trading_date(0),
      average_amount(0.0),
      turnover_threshold(0.0),
      free_share(0.0),
      pre_close(0.0),
      upper_limit(0.0),
      lower_limit(0.0),
      history_volatility_20d(0.0),
      v06_baseline(false) {}

bool StaticInputs::valid() const {
    const double max_price =
        static_cast<double>(std::numeric_limits<int>::max()) / 100.0;
    return !instrument.empty() && valid_trading_date(trading_date) &&
           std::isfinite(average_amount) && average_amount > 0.0 &&
           std::isfinite(turnover_threshold) && turnover_threshold > 0.0 &&
           std::isfinite(pre_close) && pre_close > 0.0 && pre_close <= max_price &&
           std::isfinite(upper_limit) && upper_limit > 0.0 && upper_limit <= max_price &&
           std::isfinite(lower_limit) && lower_limit > 0.0 && lower_limit <= max_price &&
           (!v06_baseline || (std::isfinite(free_share) && free_share > 0.0)) &&
           lower_limit <= upper_limit;
}

OrderEvent::OrderEvent()
    : app_sequence(0), exchange_time_us(0), local_time_us(0), price(0.0),
      volume(0), buy(false), kind(OrderKind::kLimit) {}

TradeEvent::TradeEvent()
    : app_sequence(0), exchange_time_us(0), local_time_us(0), price(0.0),
      volume(0), buy_order_id(0), sell_order_id(0), kind(TradeKind::kFill) {}

Sample::Sample()
    : instrument(),
      exchange_time_us(0),
      local_time_us(0),
      app_sequence(0),
      cut_index(0),
      row_in_stock_day(0),
      window_start_exchange_time_us(0),
      window_start_app_sequence(0),
      window_start_cut_index(0),
      last_price(0.0),
      mid_price(0.0),
      turnover(0.0),
      volume(0.0),
      amount_trigger(false),
      time_trigger(false),
      change_trigger(false) {}

SampleBuffer::SampleBuffer() : values(), count(0) {}

void SampleBuffer::clear() {
    count = 0;
}

EventTiming::EventTiming()
    : book_mutation_ns(0), sample_work_ns(0), total_runtime_ns(0) {}

void EventTiming::clear() {
    book_mutation_ns = 0;
    sample_work_ns = 0;
    total_runtime_ns = 0;
}

namespace {

std::uint64_t runtime_clock_ns() {
    struct timespec ts;
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

void finish_event_timing(EventTiming* timing, std::uint64_t start_ns) {
    if (timing == 0) {
        return;
    }
    const std::uint64_t end_ns = runtime_clock_ns();
    timing->total_runtime_ns = end_ns >= start_ns ? end_ns - start_ns : 0;
}

bool parse_two_digits(const char* text, std::size_t offset, int* result) {
    if (text == 0 || result == 0 || text[offset] < '0' || text[offset] > '9' ||
        text[offset + 1] < '0' || text[offset + 1] > '9') {
        return false;
    }
    *result = (text[offset] - '0') * 10 + text[offset + 1] - '0';
    return true;
}

int64_t date_epoch_us(int32_t date) {
    if (date <= 0) {
        return 0;
    }
    std::tm value;
    std::memset(&value, 0, sizeof(value));
    value.tm_year = date / 10000 - 1900;
    value.tm_mon = (date / 100 % 100) - 1;
    value.tm_mday = date % 100;
    const std::time_t seconds = timegm(&value);
    return seconds < 0 ? 0 : static_cast<int64_t>(seconds) * 1000000LL;
}

}  // namespace

bool parse_exchange_time_us(const char* text,
                            int32_t trading_date,
                            int64_t* result) {
    if (text == 0 || result == 0 || !valid_trading_date(trading_date)) {
        return false;
    }
    std::size_t length = 0;
    while (length < 31 && text[length] != '\0') {
        ++length;
    }
    if (length < 8 || text[2] != ':' || text[5] != ':') {
        return false;
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_two_digits(text, 0, &hour) || !parse_two_digits(text, 3, &minute) ||
        !parse_two_digits(text, 6, &second) || hour >= 24 || minute >= 60 || second >= 60) {
        return false;
    }
    int64_t fraction = 0;
    std::size_t fraction_digits = 0;
    if (length > 8) {
        if (text[8] != '.') {
            return false;
        }
        fraction_digits = length - 9;
        if (fraction_digits == 0 || fraction_digits > 6) {
            return false;
        }
        for (std::size_t i = 9; i < length; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return false;
            }
            fraction = fraction * 10 + text[i] - '0';
        }
        for (; fraction_digits < 6; ++fraction_digits) {
            fraction *= 10;
        }
    }
    const int64_t tod = (static_cast<int64_t>(hour) * 3600LL +
                         static_cast<int64_t>(minute) * 60LL + second) * 1000000LL + fraction;
    *result = date_epoch_us(trading_date) + tod;
    return true;
}

int64_t normalize_receive_time_us(int64_t receive_time,
                                  int32_t trading_date,
                                  int64_t exchange_time_us) {
    if (receive_time <= 0) {
        return exchange_time_us;
    }
    if (receive_time >= 100000000000000000LL) {
        return receive_time / 1000LL;
    }
    if (receive_time >= 500000000000000LL) {
        return receive_time;
    }
    // Backtest timestamps are HHMMSSmmmuuunnn, not epoch timestamps.
    if (receive_time >= 1000000000000LL) {
        const int hour = static_cast<int>((receive_time / 10000000000000LL) % 100);
        const int minute = static_cast<int>((receive_time / 100000000000LL) % 100);
        const int second = static_cast<int>((receive_time / 1000000000LL) % 100);
        const int millis = static_cast<int>((receive_time / 1000000LL) % 1000);
        const int micros = static_cast<int>((receive_time / 1000LL) % 1000);
        const int nanos = static_cast<int>(receive_time % 1000LL);
        const int64_t tod = (static_cast<int64_t>(hour) * 3600LL +
                             static_cast<int64_t>(minute) * 60LL + second) * 1000000LL +
                            static_cast<int64_t>(millis) * 1000LL + micros + nanos / 1000;
        return date_epoch_us(trading_date) + tod;
    }
    return exchange_time_us;
}

int64_t recover_monotonic_receive_time_us(std::uint64_t receive_mono_ns,
                                          std::uint64_t reference_mono_ns,
                                          std::uint64_t reference_realtime_ns,
                                          int32_t trading_date,
                                          int64_t exchange_time_us) {
    if (receive_mono_ns == 0U || reference_mono_ns == 0U ||
        reference_realtime_ns == 0U) {
        return exchange_time_us;
    }

    std::uint64_t estimated_realtime_ns = reference_realtime_ns;
    if (receive_mono_ns >= reference_mono_ns) {
        const std::uint64_t delta = receive_mono_ns - reference_mono_ns;
        if (delta > std::numeric_limits<std::uint64_t>::max() -
                        reference_realtime_ns) {
            return exchange_time_us;
        }
        estimated_realtime_ns += delta;
    } else {
        const std::uint64_t delta = reference_mono_ns - receive_mono_ns;
        if (delta > reference_realtime_ns) {
            return exchange_time_us;
        }
        estimated_realtime_ns -= delta;
    }

    const int64_t day_epoch_us = date_epoch_us(trading_date);
    if (day_epoch_us <= 0) {
        return exchange_time_us;
    }
    const std::uint64_t seconds = estimated_realtime_ns / 1000000000ULL;
    const std::uint64_t micros =
        (estimated_realtime_ns % 1000000000ULL) / 1000ULL;
    // Feed timestamps are China wall-clock values encoded against UTC midnight,
    // rather than UTC instants. Keep that convention for the local/exchange
    // latency guard and do not depend on the process TZ environment.
    const std::uint64_t china_seconds = (seconds + 8ULL * 3600ULL) % 86400ULL;
    return day_epoch_us + static_cast<int64_t>(china_seconds * 1000000ULL + micros);
}

struct Runtime::Impl {
    StaticInputs inputs;
    NativeBook book;
    mutable std::vector<LevelSnapshot> factor_bids;
    mutable std::vector<LevelSnapshot> factor_asks;
    std::unordered_map<int64_t, double> order_prices;
    bool pending_order;
    OrderEvent pending;
    std::vector<TradeEvent> pending_fills;
    // SZE market orders carry a zero price.  Match mdserver semantics: defer
    // the order when the current opposite best level cannot fill it, then use
    // the final linked execution price once its fills complete.
    bool deferred_market_order;
    OrderEvent deferred_market;
    int64_t deferred_market_fill_volume;
    double deferred_market_last_fill_price;
    std::vector<TradeEvent> deferred_market_fills;
    bool has_resolved_market_order;
    OrderEvent resolved_market_order;
    bool resolved_market_from_linked_fill;
    bool has_window;
    Cut window_start;
    Flow flow;
    bool has_open_state;
    double open_mid;
    double open_turnover;
    int64_t last_accepted_time_us;
    int64_t last_accepted_cut_index;
    bool has_last_accepted;
    int64_t frame_count;
    int64_t sample_count;
    int64_t row_count;
    int64_t last_ingress_sequence;
    int64_t last_ingress_time_us;
    double cumulative_turnover;
    int64_t cumulative_volume;
    double last_price;
    bool available;
    int64_t failure_sequence;
    std::string failure_reason;

    explicit Impl(const StaticInputs& value)
        : inputs(value),
          book(),
          order_prices(),
          pending_order(false),
          pending(),
          pending_fills(),
          deferred_market_order(false),
          deferred_market(),
          deferred_market_fill_volume(0),
          deferred_market_last_fill_price(0.0),
          deferred_market_fills(),
          has_resolved_market_order(false),
          resolved_market_order(),
          resolved_market_from_linked_fill(false),
          has_window(false),
          window_start(),
          flow(),
          has_open_state(false),
          open_mid(0.0),
          open_turnover(0.0),
          last_accepted_time_us(0),
          last_accepted_cut_index(0),
          has_last_accepted(false),
          frame_count(0),
          sample_count(0),
          row_count(0),
          last_ingress_sequence(0),
          last_ingress_time_us(0),
          cumulative_turnover(0.0),
          cumulative_volume(0),
          last_price(0.0),
          available(value.valid()),
          failure_sequence(0),
          failure_reason() {
        pending_fills.reserve(8);
        deferred_market_fills.reserve(8);
        if (!available) {
            failure_reason = "invalid static inputs";
        }
    }

    void clear_state() {
        book.clear();
        order_prices.clear();
        pending_order = false;
        pending_fills.clear();
        deferred_market_order = false;
        deferred_market = OrderEvent();
        deferred_market_fill_volume = 0;
        deferred_market_last_fill_price = 0.0;
        deferred_market_fills.clear();
        has_resolved_market_order = false;
        resolved_market_order = OrderEvent();
        resolved_market_from_linked_fill = false;
        has_window = false;
        window_start = Cut();
        flow.clear();
        has_open_state = false;
        open_mid = 0.0;
        open_turnover = 0.0;
        last_accepted_time_us = 0;
        last_accepted_cut_index = 0;
        has_last_accepted = false;
        frame_count = 0;
        sample_count = 0;
        row_count = 0;
        last_ingress_sequence = 0;
        last_ingress_time_us = 0;
        cumulative_turnover = 0.0;
        cumulative_volume = 0;
        last_price = 0.0;
        available = inputs.valid();
        failure_sequence = 0;
        failure_reason = available ? std::string() : std::string("invalid static inputs");
    }

    void fail(int64_t sequence, const char* reason) {
        if (!available) {
            return;
        }
        available = false;
        failure_sequence = sequence;
        failure_reason = reason == 0 ? "runtime rejected event" : reason;
        pending_order = false;
        pending_fills.clear();
    }

    Cut make_cut(int64_t cut_index, int64_t app_sequence,
                 int64_t exchange_time_us, int64_t local_time_us) const {
        Cut cut;
        cut.cut_index = cut_index;
        cut.app_sequence = app_sequence;
        cut.exchange_time_us = exchange_time_us;
        cut.local_time_us = local_time_us;
        cut.last_price = last_price;
        cut.turnover = cumulative_turnover;
        cut.volume = cumulative_volume;
        cut.bid_count = book.copy_top(true, &cut.bids);
        cut.ask_count = book.copy_top(false, &cut.asks);
        const double best_bid = cut.best_bid_price();
        const double best_ask = cut.best_ask_price();
        if (cut.has_two_sided_l1()) {
            cut.mid = (best_bid + best_ask) * 0.5;
            cut.mid_source = 0;
        } else if (last_price > 0.0) {
            cut.mid = last_price;
            cut.mid_source = 1;
        } else if (inputs.pre_close > 0.0) {
            cut.mid = inputs.pre_close;
            cut.mid_source = 1;
        } else if (best_bid > 0.0) {
            cut.mid = best_bid;
            cut.mid_source = 1;
        } else {
            cut.mid = best_ask;
            cut.mid_source = 1;
        }
        const int upper = price_tick(inputs.upper_limit);
        const int lower = price_tick(inputs.lower_limit);
        const int last_tick = price_tick(last_price);
        const int bid_tick = cut.bid_count == 0 ? 0 : cut.bids[0].tick;
        const int ask_tick = cut.ask_count == 0 ? 0 : cut.asks[0].tick;
        if (upper != 0 && cut.ask_count == 0 &&
            (last_tick == upper || bid_tick == upper)) {
            cut.limit_state = 1;
        } else if (lower != 0 && cut.bid_count == 0 &&
                   (last_tick == lower || ask_tick == lower)) {
            cut.limit_state = -1;
        } else if (upper != 0 && (upper == last_tick || upper == bid_tick || upper == ask_tick)) {
            cut.limit_state = 2;
        } else if (lower != 0 && (lower == last_tick || lower == bid_tick || lower == ask_tick)) {
            cut.limit_state = -2;
        }
        return cut;
    }

    bool mutate_trade(const TradeEvent& event, EventTiming* timing) {
        if (event.app_sequence <= 0 || event.exchange_time_us <= 0 ||
            event.local_time_us <= 0 || event.volume <= 0 ||
            !std::isfinite(event.price)) {
            return false;
        }
        if (event.kind == TradeKind::kFill) {
            if (event.volume > std::numeric_limits<int64_t>::max() - cumulative_volume) {
                return false;
            }
            const double amount = Flow::amount(event.price, static_cast<double>(event.volume));
            if (!(amount > 0.0) || !std::isfinite(cumulative_turnover + amount)) {
                return false;
            }
        }
        const std::uint64_t book_begin = timing == 0 ? 0 : runtime_clock_ns();
        const bool filled = book.fill(event);
        if (timing != 0) {
            const std::uint64_t book_end = runtime_clock_ns();
            timing->book_mutation_ns += book_end >= book_begin ? book_end - book_begin : 0;
        }
        if (!filled) {
            return false;
        }
        if (event.kind == TradeKind::kFill) {
            const double amount = Flow::amount(event.price, static_cast<double>(event.volume));
            cumulative_turnover += amount;
            cumulative_volume += event.volume;
            if (event.price > 0.0) {
                last_price = event.price;
            }
        }
        return true;
    }

    void dispatch_trade(const TradeEvent& event) {
        const int64_t cancel_id = std::max(event.buy_order_id, event.sell_order_id);
        std::unordered_map<int64_t, double>::const_iterator price_it = order_prices.find(cancel_id);
        const double cancel_price = price_it == order_prices.end() ? event.price : price_it->second;
        if (has_window && same_session(window_start.exchange_time_us, event.exchange_time_us,
                                       inputs.v06_baseline)) {
            flow.on_trade(event, cancel_price);
        }
        if (event.buy_order_id != 0 && !book.contains(event.buy_order_id)) {
            order_prices.erase(event.buy_order_id);
        }
        if (event.sell_order_id != 0 && !book.contains(event.sell_order_id)) {
            order_prices.erase(event.sell_order_id);
        }
    }

    void dispatch_order(const OrderEvent& event) {
        order_prices[event.app_sequence] = event.price;
        if (has_window && same_session(window_start.exchange_time_us, event.exchange_time_us,
                                       inputs.v06_baseline)) {
            flow.on_order(window_start, event, inputs.v06_baseline);
        }
    }

    void maybe_open(const Cut& cut) {
        if (!has_open_state && session_id(cut.exchange_time_us) != 0 && cut.has_two_sided_l1()) {
            has_open_state = true;
            open_mid = cut.mid;
            open_turnover = cut.turnover;
        }
    }

    void fill_timeline_factors(const Cut& start,
                               const Cut& current,
                               std::array<float, kFeatureCount>* output) const;
    void fill_book_factors(const Cut& current,
                           std::array<float, kFeatureCount>* output) const;
    bool maybe_emit(const Cut& cut,
                    bool* amount_trigger,
                    bool* time_trigger,
                    bool* change_trigger,
                    Sample* output);
    bool begin_order_frame(const OrderEvent& order, EventTiming* timing) {
        if (order.app_sequence <= 0 || order.exchange_time_us <= 0 ||
            order.local_time_us <= 0 || order.volume <= 0 ||
            !std::isfinite(order.price) ||
            order.app_sequence <= last_ingress_sequence ||
            order.exchange_time_us < last_ingress_time_us) {
            return false;
        }
        const std::uint64_t book_begin = timing == 0 ? 0 : runtime_clock_ns();
        const bool added = book.add(order);
        if (timing != 0) {
            const std::uint64_t book_end = runtime_clock_ns();
            timing->book_mutation_ns += book_end >= book_begin ? book_end - book_begin : 0;
        }
        if (!added) {
            return false;
        }
        last_ingress_sequence = order.app_sequence;
        last_ingress_time_us = order.exchange_time_us;
        dispatch_order(order);
        pending = order;
        pending_fills.clear();
        pending_order = true;
        return true;
    }

    bool defer_market_order(const OrderEvent& order) {
        if (order.app_sequence <= 0 || order.exchange_time_us <= 0 ||
            order.local_time_us <= 0 || order.volume <= 0 ||
            order.app_sequence <= last_ingress_sequence ||
            order.exchange_time_us < last_ingress_time_us ||
            deferred_market_order) {
            return false;
        }
        deferred_market_order = true;
        deferred_market = order;
        deferred_market_fill_volume = 0;
        deferred_market_last_fill_price = 0.0;
        deferred_market_fills.clear();
        return true;
    }

    int deferred_market_reference_tick() const {
        if (!deferred_market_order) {
            return 0;
        }
        return deferred_market.buy ? book.best_tick(false) : book.best_tick(true);
    }

    bool is_deferred_market_fill(const TradeEvent& trade) const {
        if (!deferred_market_order || trade.kind != TradeKind::kFill) {
            return false;
        }
        return deferred_market.buy
                   ? trade.buy_order_id == deferred_market.app_sequence
                   : trade.sell_order_id == deferred_market.app_sequence;
    }
    bool consume_deferred_market_cancel(const TradeEvent& trade) {
        if (!deferred_market_order || trade.kind != TradeKind::kCancel ||
            std::max(trade.buy_order_id, trade.sell_order_id) !=
                deferred_market.app_sequence) {
            return false;
        }
        if (trade.app_sequence <= last_ingress_sequence ||
            trade.exchange_time_us < last_ingress_time_us ||
            trade.local_time_us <= 0 || trade.volume <= 0 ||
            !std::isfinite(trade.price)) {
            return false;
        }
        deferred_market_order = false;
        deferred_market = OrderEvent();
        deferred_market_fill_volume = 0;
        deferred_market_last_fill_price = 0.0;
        deferred_market_fills.clear();
        last_ingress_sequence = trade.app_sequence;
        last_ingress_time_us = trade.exchange_time_us;
        return true;
    }

    void remember_resolved_market_order(const OrderEvent& order, bool from_linked_fill) {
        has_resolved_market_order = true;
        resolved_market_order = order;
        resolved_market_from_linked_fill = from_linked_fill;
    }

    bool resolve_deferred_market_order(double price, EventTiming* timing,
                                       bool from_linked_fill) {
        if (!deferred_market_order) {
            return true;
        }
        const OrderEvent original = deferred_market;
        const std::vector<TradeEvent> fills = deferred_market_fills;
        deferred_market_order = false;
        deferred_market = OrderEvent();
        deferred_market_fill_volume = 0;
        deferred_market_last_fill_price = 0.0;
        deferred_market_fills.clear();

        if (!std::isfinite(price) || price <= 0.0) {
            // mdserver publishes an unresolved zero-price market order without
            // adding it to the match book.  Preserve that behavior here.
            return true;
        }

        OrderEvent resolved = original;
        resolved.price = price;
        if (!begin_order_frame(resolved, timing)) {
            return false;
        }
        for (std::size_t i = 0; i < fills.size(); ++i) {
            if (!append_grouped_fill(fills[i], timing)) {
                return false;
            }
        }
        remember_resolved_market_order(resolved, from_linked_fill);
        return true;
    }

    bool begin_order_or_defer_market(const OrderEvent& order, EventTiming* timing) {
        // Research/offline inputs already carry the reconstructed market price.
        // Only the raw SZE feed representation uses zero and needs this state
        // machine, otherwise the same order would be inferred twice.
        if (order.kind != OrderKind::kMarket || order.price > 0.0) {
            return begin_order_frame(order, timing);
        }
        const int opposite_tick = order.buy ? book.best_tick(false) : book.best_tick(true);
        const int64_t opposite_volume = order.buy ? book.best_volume(false) : book.best_volume(true);
        if (opposite_tick > 0 && opposite_volume >= order.volume) {
            OrderEvent resolved = order;
            resolved.price = tick_price(opposite_tick);
            if (!begin_order_frame(resolved, timing)) {
                return false;
            }
            remember_resolved_market_order(resolved, false);
            return true;
        }
        return defer_market_order(order);
    }
    bool append_grouped_fill(const TradeEvent& trade, EventTiming* timing) {
        if (trade.app_sequence <= last_ingress_sequence ||
            trade.exchange_time_us < last_ingress_time_us) {
            return false;
        }
        if (!mutate_trade(trade, timing)) {
            return false;
        }
        last_ingress_sequence = trade.app_sequence;
        last_ingress_time_us = trade.exchange_time_us;
        dispatch_trade(trade);
        pending_fills.push_back(trade);
        return true;
    }
    bool process_trade_frame(const TradeEvent& trade,
                             Sample* output,
                             bool* emitted,
                             EventTiming* timing);
    void finalize_pending(Sample* output, bool* emitted) {
        if (!pending_order) {
            return;
        }
        const bool have_fills = !pending_fills.empty();
        const int64_t app_sequence = have_fills
                                         ? pending_fills.back().app_sequence
                                         : pending.app_sequence;
        const int64_t exchange_time_us = have_fills
                                             ? pending_fills.back().exchange_time_us
                                             : pending.exchange_time_us;
        const int64_t local_time_us = have_fills
                                          ? pending_fills.back().local_time_us
                                          : pending.local_time_us;
        const Cut cut = make_cut(frame_count, app_sequence,
                                 exchange_time_us, local_time_us);
        maybe_open(cut);
        *emitted = maybe_emit(cut, &output->amount_trigger, &output->time_trigger,
                              &output->change_trigger, output);
        ++frame_count;
        pending_fills.clear();
        pending_order = false;
    }
};

void Runtime::Impl::fill_timeline_factors(const Cut& start,
                                          const Cut& current,
                                          std::array<float, kFeatureCount>* output) const {
    std::array<double, kModelDepth> weighted;
    weighted_returns(start, current, &weighted);
    const double mid = current.mid;
    const double hermes = classic_hermes(current);
    (*output)[0] = static_cast<float>(safe_div(current.best_ask_price() - current.best_bid_price(), mid) * 1000.0);
    if (inputs.v06_baseline &&
        (!current.has_two_sided_l1() || std::abs(current.limit_state) == 1)) {
        (*output)[0] = 0.0f;
    }
    (*output)[1] = static_cast<float>(per_mille(mid - start.mid, mid));
    for (std::size_t i = 0; i < kModelDepth; ++i) {
        (*output)[2 + i] = static_cast<float>(weighted[i]);
    }
    const bool valid_book = std::abs(current.limit_state) != 1 && current.has_two_sided_l1();
    double ask_weighted_volume = 0.0;
    double bid_weighted_volume = 0.0;
    double ask_total = 0.0;
    double bid_total = 0.0;
    double book_notional = 0.0;
    for (std::size_t i = 0; i < kModelDepth; ++i) {
        const double ask_volume = level_volume(current.asks, current.ask_count, i);
        const double bid_volume = level_volume(current.bids, current.bid_count, i);
        const double weight = static_cast<double>(kModelDepth - i);
        ask_weighted_volume += ask_volume * weight;
        bid_weighted_volume += bid_volume * weight;
        ask_total += ask_volume;
        bid_total += bid_volume;
        book_notional += level_price(current.asks, current.ask_count, i) * ask_volume;
        book_notional += level_price(current.bids, current.bid_count, i) * bid_volume;
    }
    const double total_weighted = ask_weighted_volume + bid_weighted_volume;
    const double total_volume = ask_total + bid_total;
    (*output)[7] = valid_book && total_weighted > 0.0
                       ? static_cast<float>(ask_weighted_volume / total_weighted - 0.5)
                       : 0.0f;
    (*output)[8] = valid_book && total_volume > 0.0
                       ? static_cast<float>(ask_total / total_volume - 0.5)
                       : 0.0f;
    const double turnover = current.turnover - start.turnover;
    (*output)[9] = valid_book ? static_cast<float>(safe_div(turnover, book_notional)) : 0.0f;
    (*output)[10] = valid_book ? static_cast<float>(safe_div(current.best_ask_volume(), ask_total)) : 0.0f;
    (*output)[11] = valid_book ? static_cast<float>(safe_div(current.best_bid_volume(), bid_total)) : 0.0f;
    (*output)[12] = static_cast<float>((mid >= 0.01 && mid <= 1.0e6)
                                           ? clamp(per_mille(hermes - mid, mid), -5.0, 5.0)
                                           : 0.0);
    const double volume = static_cast<double>(current.volume - start.volume);
    double tr_sqrt = 0.0;
    if (inputs.free_share > 0.0 && volume >= 1.0e-5) {
        const double spread = start.best_ask_price() - start.best_bid_price();
        if (start.best_bid_price() > 0.0 && start.best_ask_price() > 0.0 && std::fabs(spread) >= 1.0e-6) {
            const double ratio = clamp((turnover / volume - start.mid) / spread, -0.5, 0.5);
            const double scaled = volume / inputs.free_share;
            tr_sqrt = std::sqrt(scaled * (0.5 + ratio)) - std::sqrt(scaled * (0.5 - ratio));
        }
    }
    (*output)[13] = static_cast<float>(tr_sqrt);
    (*output)[14] = static_cast<float>(std::sqrt(std::max(current.last_price * 0.15, 0.0)));
    (*output)[15] = static_cast<float>(volume_change(start, current, true));
    (*output)[16] = static_cast<float>(volume_change(start, current, false));
    const double weighted_ask = weighted_distance(current, false);
    const double weighted_bid = weighted_distance(current, true);
    const double start_weighted_ask = weighted_distance(start, false);
    const double start_weighted_bid = weighted_distance(start, true);
    (*output)[17] = static_cast<float>(per_mille(weighted_ask, mid));
    (*output)[18] = static_cast<float>(per_mille(weighted_bid, mid));
    (*output)[19] = static_cast<float>(per_mille(weighted_ask - start_weighted_ask, start.mid));
    (*output)[20] = static_cast<float>(per_mille(weighted_bid - start_weighted_bid, start.mid));
}

void Runtime::Impl::fill_book_factors(const Cut& current,
                                      std::array<float, kFeatureCount>* output) const {
    for (std::size_t i = 25; i <= 42; ++i) {
        (*output)[i] = 0.0f;
    }
    if (std::abs(current.limit_state) == 1 || !current.has_two_sided_l1()) {
        return;
    }
    const double mid_tick = (static_cast<double>(book.best_tick(true)) +
                             static_cast<double>(book.best_tick(false))) * 0.5;
    if (mid_tick <= 0.0) {
        return;
    }
    std::vector<LevelSnapshot>& bids = factor_bids;
    std::vector<LevelSnapshot>& asks = factor_asks;
    bids.clear();
    asks.clear();
    bids.reserve(book.level_count(true));
    asks.reserve(book.level_count(false));
    book.for_each_level(true, [&bids, &current](const NativeLevel& level) {
        LevelSnapshot value;
        value.tick = level.tick;
        value.volume = level.volume;
        value.count = static_cast<int64_t>(level.orders.size());
        value.age_sum_us = static_cast<int64_t>(
            static_cast<__int128>(current.exchange_time_us) * value.count - level.insert_sum_us);
        bids.push_back(value);
    });
    book.for_each_level(false, [&asks, &current](const NativeLevel& level) {
        LevelSnapshot value;
        value.tick = level.tick;
        value.volume = level.volume;
        value.count = static_cast<int64_t>(level.orders.size());
        value.age_sum_us = static_cast<int64_t>(
            static_cast<__int128>(current.exchange_time_us) * value.count - level.insert_sum_us);
        asks.push_back(value);
    });
    const double span_10 = mid_tick * 0.1;
    const double bound_ask_10 = inputs.v06_baseline ? mid_tick * 1.1 : mid_tick + span_10;
    const double bound_bid_10 = inputs.v06_baseline ? mid_tick * 0.9 : mid_tick - span_10;
    const double span_1 = mid_tick * 0.01;
    const double bound_ask_1 = inputs.v06_baseline ? mid_tick * 1.01 : mid_tick + span_1;
    const double bound_bid_1 = inputs.v06_baseline ? mid_tick * 0.99 : mid_tick - span_1;
    const double span_5 = mid_tick * 0.05;
    const double bound_ask_5 = inputs.v06_baseline ? mid_tick * 1.05 : mid_tick + span_5;
    const double bound_bid_5 = inputs.v06_baseline ? mid_tick * 0.95 : mid_tick - span_5;

    SideStats ask10;
    SideStats bid10;
    SideStats ask1;
    SideStats bid1;
    SideStats ask5;
    SideStats bid5;
    SideStats ask_band1;
    SideStats bid_band1;
    SideStats ask_band5;
    SideStats bid_band5;
    double ask_w1 = 0.0;
    double bid_w1 = 0.0;
    double ask_w5 = 0.0;
    double bid_w5 = 0.0;
    bool ask_w1_found = false;
    bool bid_w1_found = false;
    bool ask_w5_found = false;
    bool bid_w5_found = false;
    for (std::size_t i = 0; i < asks.size(); ++i) {
        const LevelSnapshot& level = asks[i];
        if (!inputs.v06_baseline && level.tick < mid_tick) {
            break;
        }
        if (level.tick < bound_ask_10) {
            ask10.volume += level.volume;
            ask10.count += level.count;
            ask10.age_sum_us += level.age_sum_us;
        }
        if (level.tick < bound_ask_1) {
            ask_band1.volume += level.volume;
            ask_band1.count += level.count;
            ask_band1.age_sum_us += level.age_sum_us;
            ask_w1_found = true;
            ask_w1 += level.volume * (1.0 - (level.tick - mid_tick) / span_1);
        }
        if (level.tick < bound_ask_5) {
            ask_band5.volume += level.volume;
            ask_band5.count += level.count;
            ask_band5.age_sum_us += level.age_sum_us;
            ask_w5_found = true;
            ask_w5 += level.volume * (1.0 - (level.tick - mid_tick) / span_5);
        }
        if (i == 0) {
            ask1.volume = level.volume;
            ask1.count = level.count;
            ask1.age_sum_us = level.age_sum_us;
        }
        if (i < 5) {
            ask5.volume += level.volume;
            ask5.count += level.count;
            ask5.age_sum_us += level.age_sum_us;
        }
    }
    for (std::size_t i = 0; i < bids.size(); ++i) {
        const LevelSnapshot& level = bids[i];
        if (!inputs.v06_baseline && level.tick > mid_tick) {
            break;
        }
        if (level.tick > bound_bid_10) {
            bid10.volume += level.volume;
            bid10.count += level.count;
            bid10.age_sum_us += level.age_sum_us;
        }
        if (level.tick > bound_bid_1) {
            bid_band1.volume += level.volume;
            bid_band1.count += level.count;
            bid_band1.age_sum_us += level.age_sum_us;
            bid_w1_found = true;
            bid_w1 += level.volume * (1.0 - (mid_tick - level.tick) / span_1);
        }
        if (level.tick > bound_bid_5) {
            bid_band5.volume += level.volume;
            bid_band5.count += level.count;
            bid_band5.age_sum_us += level.age_sum_us;
            bid_w5_found = true;
            bid_w5 += level.volume * (1.0 - (mid_tick - level.tick) / span_5);
        }
        if (i == 0) {
            bid1.volume = level.volume;
            bid1.count = level.count;
            bid1.age_sum_us = level.age_sum_us;
        }
        if (i < 5) {
            bid5.volume += level.volume;
            bid5.count += level.count;
            bid5.age_sum_us += level.age_sum_us;
        }
    }
    const auto avg_size = [](const SideStats& value) {
        return safe_div(value.volume, value.count);
    };
    const auto life = [](const SideStats& value) {
        return value.count == 0.0
                   ? 0.0
                   : static_cast<double>(value.age_sum_us) / 1000000.0 / value.count;
    };
    (*output)[25] = static_cast<float>(imbalance(ask1.count, bid1.count));
    (*output)[26] = static_cast<float>(imbalance(ask5.count, bid5.count));
    (*output)[27] = static_cast<float>(imbalance(avg_size(ask1), avg_size(bid1)));
    (*output)[28] = static_cast<float>(imbalance(avg_size(ask5), avg_size(bid5)));
    (*output)[29] = static_cast<float>(imbalance(life(ask1), life(bid1)));
    (*output)[30] = static_cast<float>(imbalance(life(ask5), life(bid5)));
    (*output)[31] = static_cast<float>(safe_div(ask_band1.volume - bid_band1.volume,
                                                ask_band1.volume + bid_band1.volume + 1.0));
    (*output)[32] = static_cast<float>(safe_div(ask_band5.volume - bid_band5.volume,
                                                ask_band5.volume + bid_band5.volume + 1.0));
    (*output)[33] = ask_w1_found && bid_w1_found
                        ? static_cast<float>(imbalance(ask_w1, bid_w1)) : 0.0f;
    (*output)[34] = ask_w5_found && bid_w5_found
                        ? static_cast<float>(imbalance(ask_w5, bid_w5)) : 0.0f;
    (*output)[35] = static_cast<float>(imbalance(avg_size(ask10), avg_size(bid10)));
    if (inputs.v06_baseline && (ask10.count == 0.0 || bid10.count == 0.0)) {
        (*output)[35] = 0.0f;
    }
    (*output)[36] = static_cast<float>(imbalance(ask10.count, bid10.count));
    (*output)[37] = static_cast<float>(imbalance(life(ask10), life(bid10)));

    bool have_max_bid = false;
    bool have_max_ask = false;
    int max_bid_tick = 0;
    int max_ask_tick = 0;
    int64_t max_bid_volume = 0;
    int64_t max_ask_volume = 0;
    for (std::size_t i = 0; i < bids.size(); ++i) {
        if (!have_max_bid || bids[i].volume >= max_bid_volume) {
            have_max_bid = true;
            max_bid_volume = bids[i].volume;
            max_bid_tick = bids[i].tick;
        }
    }
    for (std::size_t i = 0; i < asks.size(); ++i) {
        if (!have_max_ask || asks[i].volume >= max_ask_volume) {
            have_max_ask = true;
            max_ask_volume = asks[i].volume;
            max_ask_tick = asks[i].tick;
        }
    }
    if (have_max_bid) {
        (*output)[39] = static_cast<float>(safe_div(max_bid_tick - mid_tick, mid_tick));
    }
    if (have_max_ask) {
        (*output)[40] = static_cast<float>(safe_div(max_ask_tick - mid_tick, mid_tick));
    }
    if (have_max_bid && have_max_ask && max_bid_volume + max_ask_volume != 0) {
        const double ask_distance = safe_div(max_ask_tick - mid_tick, mid_tick);
        const double bid_distance = -safe_div(max_bid_tick - mid_tick, mid_tick);
        (*output)[41] = static_cast<float>(imbalance(ask_distance, bid_distance));
    }

    const double young_max = mid_tick * 0.01;
    double ask_young = 0.0;
    double bid_young = 0.0;
    book.for_each_level(false, [&ask_young, &current, mid_tick, young_max](const NativeLevel& level) {
        if (level.tick < mid_tick) {
            return;
        }
        const double delta = std::fabs(level.tick - mid_tick);
        if (delta > young_max) {
            return;
        }
        const double weight = 1.0 - delta / young_max;
        ask_young += level.young_volume * weight;
    });
    book.for_each_level(true, [&bid_young, &current, mid_tick, young_max](const NativeLevel& level) {
        if (level.tick > mid_tick) {
            return;
        }
        const double delta = std::fabs(level.tick - mid_tick);
        if (delta > young_max) {
            return;
        }
        const double weight = 1.0 - delta / young_max;
        bid_young += level.young_volume * weight;
    });
    if (ask_young != 0.0 || bid_young != 0.0) {
        (*output)[38] = static_cast<float>(imbalance(ask_young, bid_young));
    }

    const double hermes_max = mid_tick * 0.05;
    double ask_dot = 0.0;
    double ask_weight = 0.0;
    double bid_dot = 0.0;
    double bid_weight = 0.0;
    for (std::size_t i = 0; i < asks.size(); ++i) {
        const double delta = asks[i].tick - mid_tick;
        if (asks[i].tick < mid_tick || delta > hermes_max) {
            continue;
        }
        const double weight = asks[i].volume * (1.0 - delta / hermes_max);
        if (weight > 0.0) {
            ask_dot += asks[i].tick * weight;
            ask_weight += weight;
        }
    }
    for (std::size_t i = 0; i < bids.size(); ++i) {
        const double delta = mid_tick - bids[i].tick;
        if (bids[i].tick > mid_tick || delta > hermes_max) {
            continue;
        }
        const double weight = bids[i].volume * (1.0 - delta / hermes_max);
        if (weight > 0.0) {
            bid_dot += bids[i].tick * weight;
            bid_weight += weight;
        }
    }
    if (ask_weight != 0.0 || bid_weight != 0.0) {
        const double effective_ask = ask_weight != 0.0 ? ask_dot / ask_weight : asks[0].tick;
        const double effective_bid = bid_weight != 0.0 ? bid_dot / bid_weight : bids[0].tick;
        (*output)[42] = static_cast<float>(clamp(
            ((effective_ask + effective_bid) * 0.5 / mid_tick - 1.0) * 1000.0,
            -5.0, 5.0));
    }
}

bool Runtime::Impl::maybe_emit(const Cut& cut,
                               bool* amount_trigger,
                               bool* time_trigger,
                               bool* change_trigger,
                               Sample* output) {
    *amount_trigger = false;
    *time_trigger = false;
    *change_trigger = false;
    if (inputs.v06_baseline && session_id(cut.exchange_time_us, true) == 0) {
        has_window = false;
        flow.clear();
        return false;
    }
    if (!has_window) {
        window_start = cut;
        has_window = true;
        flow.clear();
        return false;
    }
    if (!same_session(window_start.exchange_time_us, cut.exchange_time_us, inputs.v06_baseline)) {
        window_start = cut;
        flow.clear();
        return false;
    }
    if (session_id(cut.exchange_time_us) == 0) {
        return false;
    }
    // The first post-open event establishes the active-session window. It is
    // a boundary marker, not a model sample; otherwise live replay advances
    // the GRU once before the formal v0.4 stream begins.
    if (!inputs.v06_baseline && time_of_day(window_start.exchange_time_us) == kMorningOpenUs &&
        cut.exchange_time_us > window_start.exchange_time_us) {
        window_start = cut;
        flow.clear();
        return false;
    }
    if (cut.cut_index <= window_start.cut_index ||
        cut.exchange_time_us == window_start.exchange_time_us) {
        return false;
    }
    *amount_trigger = flow.turnover >= inputs.turnover_threshold;
    *time_trigger = cut.exchange_time_us - window_start.exchange_time_us >= kTimeTriggerUs;
    const bool raw_change = std::fabs(cut.mid - window_start.mid) > 1.0e-6 &&
                            cut.volume - window_start.volume >= kChangeMinVolume;
    // v0.4 SZE uses the order/trade cut directly; there is no legacy
    // local-receive-time gate on the change trigger.
    *change_trigger = raw_change;
    if (!(*amount_trigger || *time_trigger || *change_trigger)) {
        return false;
    }
    if (!inputs.v06_baseline && has_last_accepted && time_of_day(cut.exchange_time_us) / 1000 <=
                                time_of_day(last_accepted_time_us) / 1000) {
        return false;
    }
    if (has_last_accepted && cut.cut_index <= last_accepted_cut_index) {
        window_start = cut;
        flow.clear();
        return false;
    }
    // Validate the completed sampling frame, after linked fills have been
    // applied. Crossing while an incoming order is still matching is normal.
    if (cut.has_two_sided_l1() && cut.best_bid_price() > cut.best_ask_price() + 1e-8) {
        fail(cut.app_sequence, "crossed best bid/ask at completed sample");
        return false;
    }
    output->factors.fill(0.0f);
    output->bid_price.fill(0.0);
    output->ask_price.fill(0.0);
    output->bid_volume.fill(0.0);
    output->ask_volume.fill(0.0);
    fill_timeline_factors(window_start, cut, &output->factors);
    const double flow_bench = std::max(inputs.free_share, 1.0);
    const bool valid_normal_flow = window_start.has_two_sided_l1() &&
                                   std::abs(window_start.limit_state) != 1;
    const double positive_order = valid_normal_flow ? flow.positive_order_amount / flow_bench : 0.0;
    const double negative_order = valid_normal_flow ? flow.negative_order_amount / flow_bench : 0.0;
    const double market_flow = (flow.market_buy_amount - flow.market_sell_amount) / flow_bench;
    const double cancel_buy = flow.cancel_buy_amount / flow_bench;
    const double cancel_sell = flow.cancel_sell_amount / flow_bench;
    const double positive_trade = flow.positive_trade_amount / flow_bench;
    const double negative_trade = flow.negative_trade_amount / flow_bench;
    output->factors[21] = static_cast<float>(clamp(
        safe_div(flow.buy_filled_volume, flow.buy_order_volume), 0.0, 1.0));
    output->factors[22] = static_cast<float>(clamp(
        safe_div(flow.sell_filled_volume, flow.sell_order_volume), 0.0, 1.0));
    output->factors[23] = static_cast<float>(safe_div(
        flow.buy_order_volume - flow.sell_order_volume,
        flow.buy_order_volume + flow.sell_order_volume + 1.0));
    const double buy_cfr = safe_div(flow.buy_filled_volume,
                                    flow.buy_filled_volume + flow.cancel_buy_amount + 1.0);
    const double sell_cfr = safe_div(flow.sell_filled_volume,
                                     flow.sell_filled_volume + flow.cancel_sell_amount + 1.0);
    output->factors[24] = static_cast<float>(safe_div(
        buy_cfr - sell_cfr, buy_cfr + sell_cfr + 1.0));
    fill_book_factors(cut, &output->factors);
    output->factors[43] = static_cast<float>(std::log1p(std::max(positive_order, 0.0)));
    output->factors[44] = static_cast<float>(std::log1p(std::max(negative_order, 0.0)));
    output->factors[45] = static_cast<float>(std::asinh(market_flow));
    output->factors[46] = static_cast<float>(std::log1p(std::max(cancel_buy, 0.0)));
    output->factors[47] = static_cast<float>(std::log1p(std::max(cancel_sell, 0.0)));
    output->factors[48] = static_cast<float>(std::log1p(std::max(positive_trade, 0.0)));
    output->factors[49] = static_cast<float>(std::log1p(std::max(negative_trade, 0.0)));

    output->instrument = inputs.instrument;
    output->exchange_time_us = cut.exchange_time_us;
    output->local_time_us = cut.local_time_us;
    output->app_sequence = cut.app_sequence;
    output->cut_index = cut.cut_index;
    output->row_in_stock_day = row_count++;
    output->window_start_exchange_time_us = window_start.exchange_time_us;
    output->window_start_app_sequence = window_start.app_sequence;
    output->window_start_cut_index = window_start.cut_index;
    output->last_price = cut.last_price;
    output->mid_price = cut.mid;
    output->turnover = cut.turnover;
    output->volume = static_cast<double>(cut.volume);
    output->amount_trigger = *amount_trigger;
    output->time_trigger = *time_trigger;
    output->change_trigger = *change_trigger;
    for (std::size_t i = 0; i < 10; ++i) {
        if (i < cut.bid_count) {
            output->bid_price[i] = tick_price(cut.bids[i].tick);
            output->bid_volume[i] = cut.bids[i].volume;
        }
        if (i < cut.ask_count) {
            output->ask_price[i] = tick_price(cut.asks[i].tick);
            output->ask_volume[i] = cut.asks[i].volume;
        }
    }
    last_accepted_time_us = cut.exchange_time_us;
    last_accepted_cut_index = cut.cut_index;
    has_last_accepted = true;
    window_start = cut;
    flow.clear();
    ++sample_count;
    return true;
}

bool Runtime::Impl::process_trade_frame(const TradeEvent& trade,
                                        Sample* output,
                                        bool* emitted,
                                        EventTiming* timing) {
    *emitted = false;
    if (trade.app_sequence <= last_ingress_sequence ||
        trade.exchange_time_us < last_ingress_time_us ||
        !mutate_trade(trade, timing)) {
        return false;
    }
    const std::uint64_t sample_begin = timing == 0 ? 0 : runtime_clock_ns();
    last_ingress_sequence = trade.app_sequence;
    last_ingress_time_us = trade.exchange_time_us;
    const Cut cut = make_cut(frame_count, trade.app_sequence,
                             trade.exchange_time_us, trade.local_time_us);
    maybe_open(cut);
    dispatch_trade(trade);
    *emitted = maybe_emit(cut, &output->amount_trigger, &output->time_trigger,
                          &output->change_trigger, output);
    ++frame_count;
    if (timing != 0) {
        const std::uint64_t sample_end = runtime_clock_ns();
        timing->sample_work_ns += sample_end >= sample_begin ? sample_end - sample_begin : 0;
    }
    return true;
}

Runtime::Runtime(const StaticInputs& inputs)
    : impl_(new Impl(inputs)) {}

Runtime::~Runtime() {}

Runtime::Runtime(Runtime&& other) noexcept
    : impl_(std::move(other.impl_)) {}

Runtime& Runtime::operator=(Runtime&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}

bool Runtime::configured() const {
    return impl_.get() != 0 && impl_->available;
}

const StaticInputs& Runtime::inputs() const {
    return impl_->inputs;
}

void Runtime::on_order(const OrderEvent& event,
                       SampleBuffer* output,
                       EventTiming* timing) {
    if (timing != 0) {
        timing->clear();
    }
    const std::uint64_t total_begin = timing == 0 ? 0 : runtime_clock_ns();
    if (output == 0) {
        finish_event_timing(timing, total_begin);
        return;
    }
    output->clear();
    if (impl_.get() == 0 || !impl_->available) {
        finish_event_timing(timing, total_begin);
        return;
    }
    if (impl_->deferred_market_order) {
        const int reference_tick = impl_->deferred_market_reference_tick();
        const std::uint64_t book_begin = timing == 0 ? 0 : runtime_clock_ns();
        const bool resolved = impl_->resolve_deferred_market_order(
            reference_tick > 0 ? tick_price(reference_tick) : 0.0, timing, false);
        if (timing != 0) {
            const std::uint64_t book_end = runtime_clock_ns();
            timing->book_mutation_ns += book_end >= book_begin ? book_end - book_begin : 0;
        }
        if (!resolved) {
            impl_->fail(event.app_sequence, "deferred market order resolution rejected");
            finish_event_timing(timing, total_begin);
            return;
        }
    }
    bool emitted = false;
    const std::uint64_t sample_begin = timing == 0 ? 0 : runtime_clock_ns();
    impl_->finalize_pending(output->count < output->values.size()
                                ? &output->values[output->count]
                                : 0,
                            &emitted);
    if (timing != 0) {
        const std::uint64_t sample_end = runtime_clock_ns();
        timing->sample_work_ns += sample_end >= sample_begin ? sample_end - sample_begin : 0;
    }
    if (emitted && output->count < output->values.size()) {
        ++output->count;
    }
    if (!impl_->begin_order_or_defer_market(event, timing)) {
        impl_->fail(event.app_sequence, "order book add rejected");
        output->clear();
    }
    finish_event_timing(timing, total_begin);
}

void Runtime::on_trade(const TradeEvent& event,
                       SampleBuffer* output,
                       EventTiming* timing) {
    if (timing != 0) {
        timing->clear();
    }
    const std::uint64_t total_begin = timing == 0 ? 0 : runtime_clock_ns();
    if (output == 0) {
        finish_event_timing(timing, total_begin);
        return;
    }
    output->clear();
    if (impl_.get() == 0 || !impl_->available) {
        finish_event_timing(timing, total_begin);
        return;
    }
    if (impl_->deferred_market_order) {
        if (impl_->is_deferred_market_fill(event)) {
            if (event.volume > std::numeric_limits<int64_t>::max() -
                                   impl_->deferred_market_fill_volume) {
                impl_->fail(event.app_sequence, "deferred market fill volume overflow");
                finish_event_timing(timing, total_begin);
                return;
            }
            impl_->deferred_market_fills.push_back(event);
            impl_->deferred_market_fill_volume += event.volume;
            impl_->deferred_market_last_fill_price = event.price;
            if (impl_->deferred_market_fill_volume < impl_->deferred_market.volume) {
                finish_event_timing(timing, total_begin);
                return;
            }
            if (!impl_->resolve_deferred_market_order(
                    impl_->deferred_market_last_fill_price, timing, true)) {
                impl_->fail(event.app_sequence, "deferred market fill replay rejected");
                output->clear();
            }
            finish_event_timing(timing, total_begin);
            return;
        }

        if (event.kind == TradeKind::kCancel &&
            std::max(event.buy_order_id, event.sell_order_id) ==
                impl_->deferred_market.app_sequence) {
            if (impl_->deferred_market_fills.empty()) {
                if (!impl_->consume_deferred_market_cancel(event)) {
                    impl_->fail(event.app_sequence, "deferred market cancel rejected");
                }
                finish_event_timing(timing, total_begin);
                return;
            }
            // A partial-fill-then-cancel must apply all linked fills before
            // removing the remainder. Dropping the buffered fills leaves
            // already-executed opposite-side orders in the book.
            if (!impl_->resolve_deferred_market_order(
                    impl_->deferred_market_last_fill_price, timing, true)) {
                impl_->fail(event.app_sequence, "partial market fill replay before cancel rejected");
                output->clear();
                finish_event_timing(timing, total_begin);
                return;
            }
            // Continue through the ordinary cancel path for the remainder.
        }

        const int64_t related_order = std::max(event.buy_order_id, event.sell_order_id);
        const int reference_tick = impl_->deferred_market_reference_tick();
        if (related_order > impl_->deferred_market.app_sequence ||
            event.kind == TradeKind::kCancel) {
            if (!impl_->resolve_deferred_market_order(
                    reference_tick > 0 ? tick_price(reference_tick) : 0.0, timing, false)) {
                impl_->fail(event.app_sequence, "deferred market reference resolution rejected");
                finish_event_timing(timing, total_begin);
                return;
            }
        }
    }
    if (impl_->pending_order && event.kind == TradeKind::kFill &&
        std::max(event.buy_order_id, event.sell_order_id) == impl_->pending.app_sequence &&
        // SZE can report one market order as multiple linked fills spanning
        // several exchange timestamps.  The reference replay treats them as
        // one OrderWithFills frame; limit orders retain the original same-time
        // grouping rule.
        (impl_->pending.kind == OrderKind::kMarket ||
         event.exchange_time_us == impl_->pending.exchange_time_us)) {
        if (!impl_->append_grouped_fill(event, timing)) {
            impl_->fail(event.app_sequence, "grouped fill book mutation rejected");
            output->clear();
        }
        finish_event_timing(timing, total_begin);
        return;
    }
    bool emitted = false;
    const std::uint64_t sample_begin = timing == 0 ? 0 : runtime_clock_ns();
    impl_->finalize_pending(&output->values[0], &emitted);
    if (timing != 0) {
        const std::uint64_t sample_end = runtime_clock_ns();
        timing->sample_work_ns += sample_end >= sample_begin ? sample_end - sample_begin : 0;
    }
    if (emitted) {
        output->count = 1;
    }
    Sample* current_output = output->count < output->values.size()
                                 ? &output->values[output->count]
                                 : 0;
    bool current_emitted = false;
    if (current_output != 0) {
        if (!impl_->process_trade_frame(event, current_output, &current_emitted, timing)) {
            impl_->fail(event.app_sequence, "trade book mutation rejected");
            output->clear();
            finish_event_timing(timing, total_begin);
            return;
        }
    }
    if (current_emitted) {
        ++output->count;
    }
    finish_event_timing(timing, total_begin);
}

void Runtime::flush(SampleBuffer* output) {
    if (output == 0) {
        return;
    }
    output->clear();
    if (impl_.get() == 0 || !impl_->available) {
        return;
    }
    bool emitted = false;
    impl_->finalize_pending(&output->values[0], &emitted);
    if (emitted) {
        output->count = 1;
    }
}

bool Runtime::take_resolved_market_order(OrderEvent* event, bool* from_linked_fill) {
    if (event == 0 || impl_.get() == 0 || !impl_->has_resolved_market_order) {
        return false;
    }
    *event = impl_->resolved_market_order;
    if (from_linked_fill != 0) {
        *from_linked_fill = impl_->resolved_market_from_linked_fill;
    }
    impl_->has_resolved_market_order = false;
    impl_->resolved_market_order = OrderEvent();
    impl_->resolved_market_from_linked_fill = false;
    return true;
}

void Runtime::reset() {
    if (impl_.get() != 0) {
        impl_->clear_state();
    }
}

void Runtime::invalidate() {
    if (impl_.get() != 0) {
        impl_->fail(0, "runtime invalidated by caller");
    }
}

std::size_t Runtime::frame_count() const {
    return impl_.get() == 0 ? 0 : static_cast<std::size_t>(impl_->frame_count);
}

std::size_t Runtime::sample_count() const {
    return impl_.get() == 0 ? 0 : static_cast<std::size_t>(impl_->sample_count);
}

bool Runtime::available() const {
    return impl_.get() != 0 && impl_->available;
}

int64_t Runtime::failure_sequence() const {
    return impl_.get() == 0 ? 0 : impl_->failure_sequence;
}

const std::string& Runtime::failure_reason() const {
    static const std::string empty;
    return impl_.get() == 0 ? empty : impl_->failure_reason;
}

}  // namespace mix153060
