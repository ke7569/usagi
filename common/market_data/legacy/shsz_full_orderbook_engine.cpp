#include "common/market_data/legacy/shsz_full_orderbook_engine.h"

#include <algorithm>
#include <cstring>

namespace {

inline uint32_t time_string_to_ms(const char* event_time) {
    if (event_time == nullptr || event_time[0] == '\0') {
        return 0;
    }

    const size_t len = std::strlen(event_time);
    if (len < 8) {
        return 0;
    }

    const uint32_t hours = static_cast<uint32_t>((event_time[0] - '0') * 10 + (event_time[1] - '0'));
    const uint32_t minutes = static_cast<uint32_t>((event_time[3] - '0') * 10 + (event_time[4] - '0'));
    const uint32_t seconds = static_cast<uint32_t>((event_time[6] - '0') * 10 + (event_time[7] - '0'));

    uint32_t milliseconds = 0;
    if (len >= 12 && event_time[8] == '.') {
        milliseconds = static_cast<uint32_t>((event_time[9] - '0') * 100 +
                                             (event_time[10] - '0') * 10 +
                                             (event_time[11] - '0'));
    }

    return ((hours * 60U + minutes) * 60U + seconds) * 1000U + milliseconds;
}

template <typename Iterator>
ShSzBookAggregateSummary summarize_levels(Iterator begin,
                                          Iterator end,
                                          size_t max_levels,
                                          int mid_price,
                                          int max_basis_points,
                                          bool is_sell,
                                          uint32_t now_time_ms) {
    ShSzBookAggregateSummary summary;
    if (begin == end) {
        return summary;
    }

    const bool use_level_limit = max_levels > 0;
    const bool use_mid_limit = mid_price > 0 && max_basis_points > 0;
    const double lower_bound = use_mid_limit
                                   ? static_cast<double>(mid_price) * (10000.0 - max_basis_points) / 10000.0
                                   : 0.0;
    const double upper_bound = use_mid_limit
                                   ? static_cast<double>(mid_price) * (10000.0 + max_basis_points) / 10000.0
                                   : 0.0;

    size_t level_index = 0;
    for (Iterator it = begin; it != end; ++it) {
        const ShSzFullOrderBookLevel& level = it->second;
        if (use_mid_limit) {
            if (is_sell) {
                if (static_cast<double>(level.price()) >= upper_bound) {
                    break;
                }
            } else {
                if (static_cast<double>(level.price()) <= lower_bound) {
                    break;
                }
            }
        }
        if (use_level_limit && level_index >= max_levels) {
            break;
        }

        if (!summary.valid) {
            summary.best_price = level.price();
            summary.best_volume = level.total_volume();
            summary.valid = true;
        }
        summary.level_count += 1;
        summary.total_volume += level.total_volume();
        summary.total_order_count += level.order_count();
        summary.total_window_volume += level.window_volume(now_time_ms);
        summary.total_create_time_ms += level.total_create_time_ms();
        summary.total_amount += level.total_amount();
        if (level.total_volume() > summary.max_level_volume) {
            summary.max_level_volume = level.total_volume();
            summary.max_level_price = level.price();
        }
        ++level_index;
    }

    return summary;
}

inline void accumulate_order_sum(const ShSzFullOrderBookLevel& level,
                                 uint32_t now_time_ms,
                                 ShSzFullOrderSum* out) {
    if (out == 0) {
        return;
    }
    out->volume_sum += static_cast<double>(level.total_volume());
    out->count_sum += static_cast<int64_t>(level.order_count());
    out->tsc_sum += static_cast<int64_t>(level.total_create_time_ms());
    out->amt_sum += level.total_amount();
    out->window_volume_sum += static_cast<double>(level.window_volume(now_time_ms));
}

inline void update_max_level(const ShSzFullOrderBookLevel& level,
                             int64_t* max_volume,
                             double* max_level_price) {
    if (max_volume == 0 || max_level_price == 0) {
        return;
    }
    if (*max_volume < level.total_volume()) {
        *max_volume = static_cast<int64_t>(level.total_volume());
        *max_level_price = static_cast<double>(level.price()) / PRICE_MULTIPLIER;
    }
}

inline void accumulate_visible_level(const ShSzVisibleBookLevel& level,
                                     ShSzFullOrderSum* out) {
    if (out == 0 || !level.valid) {
        return;
    }
    out->volume_sum += static_cast<double>(level.total_volume);
    out->count_sum += static_cast<int64_t>(level.order_count);
    out->tsc_sum += static_cast<int64_t>(level.total_create_time_ms);
    out->amt_sum += static_cast<double>(level.price) * static_cast<double>(level.total_volume) / PRICE_MULTIPLIER;
    out->window_volume_sum += static_cast<double>(level.window_volume);
}

template <size_t N>
void accumulate_visible_prefix(const std::array<ShSzVisibleBookLevel, PRICE_LEVEL>& levels,
                               ShSzFullOrderSum* out) {
    const size_t max_levels = std::min(static_cast<size_t>(N), levels.size());
    for (size_t i = 0; i < max_levels; ++i) {
        if (!levels[i].valid) {
            break;
        }
        accumulate_visible_level(levels[i], out);
    }
}

inline double clamp_double(double value, double min_value, double max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

} // namespace

ShSzSlidingWindowVolume::ShSzSlidingWindowVolume()
    : mTotalVolume(0), mNowSec(0) {
    clear();
}

bool ShSzSlidingWindowVolume::add(uint32_t event_time_ms, int volume) {
    if (volume <= 0) {
        return true;
    }

    const uint32_t event_sec = event_time_ms / 1000U;
    advance_to(event_sec);
    if (!in_window(event_sec)) {
        return true;
    }

    Bucket& target = bucket(event_sec);
    if (target.tick_sec != event_sec) {
        mTotalVolume -= target.volume;
        target.volume = 0;
        target.tick_sec = event_sec;
    }

    target.volume += volume;
    mTotalVolume += volume;
    return true;
}

bool ShSzSlidingWindowVolume::erase(uint32_t event_time_ms, int volume) {
    if (volume <= 0) {
        return true;
    }

    const uint32_t event_sec = event_time_ms / 1000U;
    if (event_sec > mNowSec) {
        advance_to(event_sec);
    }
    if (!in_window(event_sec)) {
        return true;
    }

    Bucket& target = bucket(event_sec);
    if (target.tick_sec != event_sec) {
        return true;
    }
    if (target.volume < volume) {
        return false;
    }

    target.volume -= volume;
    mTotalVolume -= volume;
    return true;
}

int ShSzSlidingWindowVolume::total(uint32_t now_time_ms) const {
    advance_to(now_time_ms / 1000U);
    return mTotalVolume;
}

void ShSzSlidingWindowVolume::clear() {
    for (size_t i = 0; i < mRing.size(); ++i) {
        mRing[i].tick_sec = 0;
        mRing[i].volume = 0;
    }
    mTotalVolume = 0;
    mNowSec = 0;
}

void ShSzSlidingWindowVolume::advance_to(uint32_t now_sec) const {
    if (now_sec <= mNowSec) {
        return;
    }

    const uint32_t old_sec = mNowSec;
    mNowSec = now_sec;
    const uint32_t gap = now_sec - old_sec;
    if (gap >= kRingSize) {
        for (size_t i = 0; i < mRing.size(); ++i) {
            mRing[i].tick_sec = 0;
            mRing[i].volume = 0;
        }
        mTotalVolume = 0;
        return;
    }

    for (uint32_t sec = old_sec + 1; sec <= now_sec; ++sec) {
        const uint32_t expired_sec = sec - kRingSize;
        Bucket& expired = bucket(expired_sec);
        if (expired.tick_sec == expired_sec) {
            mTotalVolume -= expired.volume;
            expired.tick_sec = 0;
            expired.volume = 0;
        }
    }
}

bool ShSzSlidingWindowVolume::in_window(uint32_t event_sec) const {
    return event_sec <= mNowSec && (mNowSec - event_sec) <= kWindowSec;
}

ShSzSlidingWindowVolume::Bucket& ShSzSlidingWindowVolume::bucket(uint32_t tick_sec) const {
    return mRing[tick_sec % kRingSize];
}

ShSzFullOrderRecord::ShSzFullOrderRecord()
    : quote_tag(0),
      price(0),
      total_volume(0),
      remaining_volume(0),
      is_sell(false),
      create_time_ms(0) {
}

ShSzVisibleBookLevel::ShSzVisibleBookLevel()
    : price(0),
      total_volume(0),
      order_count(0),
      total_create_time_ms(0),
      window_volume(0),
      valid(false) {
}

ShSzBookAggregateSummary::ShSzBookAggregateSummary()
    : best_price(0),
      best_volume(0),
      level_count(0),
      total_volume(0),
      total_order_count(0),
      total_window_volume(0),
      max_level_volume(0),
      max_level_price(0),
      total_create_time_ms(0),
      total_amount(0.0),
      valid(false) {
}

ShSzFullOrderBookSummary::ShSzFullOrderBookSummary()
    : instrument_id_value(0.0),
      mid_price(0),
      total_order_count(0),
      bid(),
      ask() {
}

ShSzFullOrderBookLevel::ShSzFullOrderBookLevel(int price)
    : mPrice(price),
      mTotalVolume(0),
      mTotalCreateTimeMs(0) {
    mOrders.reserve(256);
}

bool ShSzFullOrderBookLevel::add_order(const ShSzFullOrderRecord& order) {
    if (order.remaining_volume <= 0) {
        return false;
    }
    if (mOrders.find(order.quote_tag) != mOrders.end()) {
        return false;
    }

    mOrders.insert(std::make_pair(order.quote_tag, order));
    mTotalVolume += order.remaining_volume;
    mTotalCreateTimeMs += order.create_time_ms;
    return mWindowVolume.add(order.create_time_ms, order.remaining_volume);
}

bool ShSzFullOrderBookLevel::reduce_order(long quote_tag, int volume, bool erase_when_flat, bool* removed_order) {
    if (volume <= 0) {
        return false;
    }

    if (removed_order != 0) {
        *removed_order = false;
    }

    std::unordered_map<long, ShSzFullOrderRecord>::iterator it = mOrders.find(quote_tag);
    if (it == mOrders.end()) {
        return false;
    }
    if (it->second.remaining_volume < volume) {
        return false;
    }

    mTotalVolume -= volume;
    it->second.remaining_volume -= volume;
    const bool window_ok = mWindowVolume.erase(it->second.create_time_ms, volume);
    if (it->second.remaining_volume == 0 && erase_when_flat) {
        if (removed_order != 0) {
            *removed_order = true;
        }
        mTotalCreateTimeMs -= it->second.create_time_ms;
        mOrders.erase(it);
        return window_ok;
    }
    return window_ok;
}

bool ShSzFullOrderBookLevel::remove_order(long quote_tag) {
    std::unordered_map<long, ShSzFullOrderRecord>::iterator it = mOrders.find(quote_tag);
    if (it == mOrders.end()) {
        return false;
    }

    mTotalVolume -= it->second.remaining_volume;
    mTotalCreateTimeMs -= it->second.create_time_ms;
    const bool ok = mWindowVolume.erase(it->second.create_time_ms, it->second.remaining_volume);
    mOrders.erase(it);
    return ok;
}

bool ShSzFullOrderBookLevel::get_order(long quote_tag, ShSzFullOrderRecord* out) const {
    std::unordered_map<long, ShSzFullOrderRecord>::const_iterator it = mOrders.find(quote_tag);
    if (it == mOrders.end()) {
        return false;
    }
    if (out != 0) {
        *out = it->second;
    }
    return true;
}

int ShSzFullOrderBookLevel::price() const {
    return mPrice;
}

int ShSzFullOrderBookLevel::total_volume() const {
    return mTotalVolume;
}

int ShSzFullOrderBookLevel::order_count() const {
    return static_cast<int>(mOrders.size());
}

uint64_t ShSzFullOrderBookLevel::total_create_time_ms() const {
    return mTotalCreateTimeMs;
}

double ShSzFullOrderBookLevel::total_amount() const {
    return static_cast<double>(mPrice) * static_cast<double>(mTotalVolume) / PRICE_MULTIPLIER;
}

int ShSzFullOrderBookLevel::window_volume(uint32_t now_time_ms) const {
    return mWindowVolume.total(now_time_ms);
}

bool ShSzFullOrderBookLevel::empty() const {
    return mOrders.empty();
}

void ShSzFullOrderBookLevel::clear() {
    mTotalVolume = 0;
    mTotalCreateTimeMs = 0;
    mOrders.clear();
    mWindowVolume.clear();
}

ShSzFullOrderBookEngine::ShSzFullOrderBookEngine()
    : mInstrumentIdValue(0.0),
      mBidOrderCount(0),
      mAskOrderCount(0),
      mBidMaxLevelVolume(0),
      mAskMaxLevelVolume(0),
      mBidMaxLevelPrice(0.0),
      mAskMaxLevelPrice(0.0) {
    mOrderLocators.reserve(32768);
}

void ShSzFullOrderBookEngine::set_instrument_id_value(double instrument_id_value) {
    mInstrumentIdValue = instrument_id_value;
}

double ShSzFullOrderBookEngine::instrument_id_value() const {
    return mInstrumentIdValue;
}

bool ShSzFullOrderBookEngine::add_order(long quote_tag,
                                        bool is_sell,
                                        int price,
                                        int volume,
                                        uint32_t event_time_ms) {
    if (quote_tag == 0 || price <= 0 || volume <= 0) {
        return false;
    }
    if (mOrderLocators.find(quote_tag) != mOrderLocators.end()) {
        return false;
    }

    ShSzFullOrderRecord order;
    order.quote_tag = quote_tag;
    order.price = price;
    order.total_volume = volume;
    order.remaining_volume = volume;
    order.is_sell = is_sell;
    order.create_time_ms = event_time_ms;

    if (is_sell) {
        AskBook::iterator level_it = mAskBook.find(price);
        if (level_it == mAskBook.end()) {
            level_it = mAskBook.insert(std::make_pair(price, ShSzFullOrderBookLevel(price))).first;
        }
        if (!level_it->second.add_order(order)) {
            return false;
        }
        update_ask_max_level_on_add(price, level_it->second.total_volume());
        mAskOrderCount += 1;
        refresh_ask_side_cache(event_time_ms);
    } else {
        BidBook::iterator level_it = mBidBook.find(price);
        if (level_it == mBidBook.end()) {
            level_it = mBidBook.insert(std::make_pair(price, ShSzFullOrderBookLevel(price))).first;
        }
        if (!level_it->second.add_order(order)) {
            return false;
        }
        update_bid_max_level_on_add(price, level_it->second.total_volume());
        mBidOrderCount += 1;
        refresh_bid_side_cache(event_time_ms);
    }

    OrderLocator locator;
    locator.is_sell = is_sell;
    locator.price = price;
    mOrderLocators.insert(std::make_pair(quote_tag, locator));
    return true;
}

bool ShSzFullOrderBookEngine::execute_order(long quote_tag, int volume) {
    return reduce_order(quote_tag, volume, true);
}

bool ShSzFullOrderBookEngine::cancel_order(long quote_tag, int volume) {
    return reduce_order(quote_tag, volume, true);
}

bool ShSzFullOrderBookEngine::remove_order(long quote_tag) {
    std::unordered_map<long, OrderLocator>::iterator locator_it = mOrderLocators.find(quote_tag);
    if (locator_it == mOrderLocators.end()) {
        return false;
    }

    if (locator_it->second.is_sell) {
        AskBook::iterator level_it = mAskBook.find(locator_it->second.price);
        if (level_it == mAskBook.end() || !level_it->second.remove_order(quote_tag)) {
            return false;
        }
        mAskOrderCount -= 1;
        if (level_it->second.empty()) {
            mAskBook.erase(level_it);
            if (mAskMaxLevelPrice == static_cast<double>(locator_it->second.price) / PRICE_MULTIPLIER) {
                rescan_ask_max_level();
            }
        } else {
            const int remaining_level_volume = level_it->second.total_volume();
            if (mAskMaxLevelPrice == static_cast<double>(locator_it->second.price) / PRICE_MULTIPLIER &&
                remaining_level_volume < mAskMaxLevelVolume) {
                rescan_ask_max_level();
            }
        }
        refresh_ask_side_cache(0);
    } else {
        BidBook::iterator level_it = mBidBook.find(locator_it->second.price);
        if (level_it == mBidBook.end() || !level_it->second.remove_order(quote_tag)) {
            return false;
        }
        mBidOrderCount -= 1;
        if (level_it->second.empty()) {
            mBidBook.erase(level_it);
            if (mBidMaxLevelPrice == static_cast<double>(locator_it->second.price) / PRICE_MULTIPLIER) {
                rescan_bid_max_level();
            }
        } else {
            const int remaining_level_volume = level_it->second.total_volume();
            if (mBidMaxLevelPrice == static_cast<double>(locator_it->second.price) / PRICE_MULTIPLIER &&
                remaining_level_volume < mBidMaxLevelVolume) {
                rescan_bid_max_level();
            }
        }
        refresh_bid_side_cache(0);
    }

    mOrderLocators.erase(locator_it);
    return true;
}

bool ShSzFullOrderBookEngine::get_order(long quote_tag, ShSzFullOrderRecord* out) const {
    std::unordered_map<long, OrderLocator>::const_iterator locator_it = mOrderLocators.find(quote_tag);
    if (locator_it == mOrderLocators.end()) {
        return false;
    }

    if (locator_it->second.is_sell) {
        AskBook::const_iterator level_it = mAskBook.find(locator_it->second.price);
        if (level_it == mAskBook.end()) {
            return false;
        }
        return level_it->second.get_order(quote_tag, out);
    }

    BidBook::const_iterator level_it = mBidBook.find(locator_it->second.price);
    if (level_it == mBidBook.end()) {
        return false;
    }
    return level_it->second.get_order(quote_tag, out);
}

bool ShSzFullOrderBookEngine::has_order(long quote_tag) const {
    return mOrderLocators.find(quote_tag) != mOrderLocators.end();
}

size_t ShSzFullOrderBookEngine::bid_level_count() const {
    return mBidBook.size();
}

size_t ShSzFullOrderBookEngine::ask_level_count() const {
    return mAskBook.size();
}

size_t ShSzFullOrderBookEngine::order_count() const {
    return mOrderLocators.size();
}

int ShSzFullOrderBookEngine::best_bid_price() const {
    return mBidBook.empty() ? 0 : mBidBook.rbegin()->first;
}

int ShSzFullOrderBookEngine::best_ask_price() const {
    return mAskBook.empty() ? 0 : mAskBook.begin()->first;
}

int ShSzFullOrderBookEngine::mid_price() const {
    if (mBidBook.empty() || mAskBook.empty()) {
        return 0;
    }
    return (mBidBook.rbegin()->first + mAskBook.begin()->first) / 2;
}

int ShSzFullOrderBookEngine::best_bid_volume() const {
    return mBidBook.empty() ? 0 : mBidBook.rbegin()->second.total_volume();
}

int ShSzFullOrderBookEngine::best_ask_volume() const {
    return mAskBook.empty() ? 0 : mAskBook.begin()->second.total_volume();
}

ShSzVisibleBook ShSzFullOrderBookEngine::snapshot_visible_book(uint32_t now_time_ms) const {
    ShSzVisibleBook view = mVisibleBookCache;
    refresh_visible_window_volume(mBidBook, &view.bids, now_time_ms);
    refresh_visible_window_volume(mAskBook, &view.asks, now_time_ms);
    return view;
}

ShSzBookAggregateSummary ShSzFullOrderBookEngine::snapshot_bid_summary(uint32_t now_time_ms) const {
    return summarize_levels(mBidBook.rbegin(), mBidBook.rend(), 0, 0, 0, false, now_time_ms);
}

ShSzBookAggregateSummary ShSzFullOrderBookEngine::snapshot_ask_summary(uint32_t now_time_ms) const {
    return summarize_levels(mAskBook.begin(), mAskBook.end(), 0, 0, 0, true, now_time_ms);
}

ShSzFullOrderBookSummary ShSzFullOrderBookEngine::snapshot_summary(uint32_t now_time_ms) const {
    ShSzFullOrderBookSummary summary;
    summary.instrument_id_value = mInstrumentIdValue;
    summary.mid_price = mid_price();
    summary.bid = snapshot_bid_summary(now_time_ms);
    summary.ask = snapshot_ask_summary(now_time_ms);
    summary.total_order_count = summary.bid.total_order_count + summary.ask.total_order_count;
    return summary;
}

ShSzFullOrderBookStateSnapshot ShSzFullOrderBookEngine::snapshot_state(uint32_t now_time_ms) const {
    ShSzFullOrderBookStateSnapshot snapshot;
    snapshot.visible_book = snapshot_visible_book(now_time_ms);
    snapshot.summary = snapshot_lightweight_summary();
    return snapshot;
}

ShSzFullOb ShSzFullOrderBookEngine::snapshot_full_orderbook_aggregate(uint32_t now_time_ms) const {
    ShSzFullOb full_ob;
    const ShSzVisibleBook visible_book = snapshot_visible_book(now_time_ms);

    full_ob.ask_total_count = static_cast<int64_t>(mAskBook.size());
    full_ob.bid_total_count = static_cast<int64_t>(mBidBook.size());
    full_ob.ask_max_volume = static_cast<int64_t>(mAskMaxLevelVolume);
    full_ob.bid_max_volume = static_cast<int64_t>(mBidMaxLevelVolume);
    full_ob.ask_max_level_price = mAskMaxLevelPrice;
    full_ob.bid_max_level_price = mBidMaxLevelPrice;

    if (mAskBook.empty() || mBidBook.empty()) {
        return full_ob;
    }

    full_ob.valid = true;
    full_ob.mp = static_cast<double>(mAskBook.begin()->first + mBidBook.rbegin()->first) / 2.0 / PRICE_MULTIPLIER;
    if (visible_book.asks[0].valid) {
        full_ob.ask_level1.price = static_cast<double>(visible_book.asks[0].price);
        accumulate_visible_level(visible_book.asks[0], &full_ob.ask_level1);
    }
    if (visible_book.bids[0].valid) {
        full_ob.bid_level1.price = static_cast<double>(visible_book.bids[0].price);
        accumulate_visible_level(visible_book.bids[0], &full_ob.bid_level1);
    }
    accumulate_visible_prefix<5>(visible_book.asks, &full_ob.ask_level5);
    accumulate_visible_prefix<5>(visible_book.bids, &full_ob.bid_level5);

    int cur_level = 1;
    for (AskBook::const_iterator it = mAskBook.begin(); it != mAskBook.end(); ++it, ++cur_level) {
        const ShSzFullOrderBookLevel& level = it->second;
        const double level_price = static_cast<double>(level.price()) / PRICE_MULTIPLIER;
        bool need_more = false;

        if (level_price < full_ob.mp * 1.01) {
            accumulate_order_sum(level, now_time_ms, &full_ob.ask_01);
        }
        if (level_price < full_ob.mp * 1.05) {
            accumulate_order_sum(level, now_time_ms, &full_ob.ask_05);
        }
        if (level_price < full_ob.mp * 1.10) {
            accumulate_order_sum(level, now_time_ms, &full_ob.ask_10);
            need_more = true;
        }
        if (cur_level <= 5) {
            need_more = true;
        }
        if (!need_more) {
            break;
        }
    }

    cur_level = 1;
    for (BidBook::const_reverse_iterator it = mBidBook.rbegin(); it != mBidBook.rend(); ++it, ++cur_level) {
        const ShSzFullOrderBookLevel& level = it->second;
        const double level_price = static_cast<double>(level.price()) / PRICE_MULTIPLIER;
        bool need_more = false;

        if (level_price > full_ob.mp * 0.99) {
            accumulate_order_sum(level, now_time_ms, &full_ob.bid_01);
        }
        if (level_price > full_ob.mp * 0.95) {
            accumulate_order_sum(level, now_time_ms, &full_ob.bid_05);
        }
        if (level_price > full_ob.mp * 0.90) {
            accumulate_order_sum(level, now_time_ms, &full_ob.bid_10);
            need_more = true;
        }
        if (cur_level <= 5) {
            need_more = true;
        }
        if (!need_more) {
            break;
        }
    }

    return full_ob;
}

double ShSzFullOrderBookEngine::young_orderbook_imbalance(int max_basis_points, uint32_t now_time_ms) const {
    if (max_basis_points <= 0 || mAskBook.empty() || mBidBook.empty()) {
        return 0.0;
    }

    const double mid_price_value =
        static_cast<double>(mAskBook.begin()->first + mBidBook.rbegin()->first) / 2.0;
    if (mid_price_value <= 0.0) {
        return 0.0;
    }
    const double max_distance = mid_price_value * static_cast<double>(max_basis_points) / 10000.0;
    if (max_distance <= 0.0) {
        return 0.0;
    }

    double ask_sum = 0.0;
    double bid_sum = 0.0;

    for (BidBook::const_reverse_iterator it = mBidBook.rbegin(); it != mBidBook.rend(); ++it) {
        const double price = static_cast<double>(it->second.price());
        if (price >= mid_price_value) {
            continue;
        }
        const double distance = mid_price_value - price;
        if (distance > max_distance) {
            break;
        }
        const double price_weight = 1.0 - distance / max_distance;
        bid_sum += static_cast<double>(it->second.window_volume(now_time_ms)) * price_weight;
    }

    for (AskBook::const_iterator it = mAskBook.begin(); it != mAskBook.end(); ++it) {
        const double price = static_cast<double>(it->second.price());
        if (price <= mid_price_value) {
            continue;
        }
        const double distance = price - mid_price_value;
        if (distance > max_distance) {
            break;
        }
        const double price_weight = 1.0 - distance / max_distance;
        ask_sum += static_cast<double>(it->second.window_volume(now_time_ms)) * price_weight;
    }

    if (ask_sum <= 1e-6 && bid_sum <= 1e-6) {
        return 0.0;
    }
    return (ask_sum - bid_sum) / (ask_sum + bid_sum);
}

double ShSzFullOrderBookEngine::fix_dist_hermes(int max_basis_points, uint32_t now_time_ms) const {
    (void)now_time_ms;
    if (max_basis_points <= 0 || mAskBook.empty() || mBidBook.empty()) {
        return 0.0;
    }

    const double mid_price_value =
        static_cast<double>(mAskBook.begin()->first + mBidBook.rbegin()->first) / 2.0;
    if (mid_price_value <= 0.0) {
        return 0.0;
    }
    const double max_distance = mid_price_value * static_cast<double>(max_basis_points) / 10000.0;
    if (max_distance <= 0.0) {
        return 0.0;
    }

    double weighted_ask_sum = 0.0;
    double ask_weight = 0.0;
    double weighted_bid_sum = 0.0;
    double bid_weight = 0.0;
    const double best_ask = static_cast<double>(mAskBook.begin()->first);
    const double best_bid = static_cast<double>(mBidBook.rbegin()->first);

    for (BidBook::const_reverse_iterator it = mBidBook.rbegin(); it != mBidBook.rend(); ++it) {
        const double price = static_cast<double>(it->second.price());
        if (price > mid_price_value) {
            continue;
        }
        const double distance = mid_price_value - price;
        if (distance > max_distance) {
            break;
        }
        const double price_weight = 1.0 - distance / max_distance;
        if (price_weight <= 0.0) {
            break;
        }
        const double weight = price_weight * static_cast<double>(it->second.total_volume());
        weighted_bid_sum += weight * price;
        bid_weight += weight;
    }

    for (AskBook::const_iterator it = mAskBook.begin(); it != mAskBook.end(); ++it) {
        const double price = static_cast<double>(it->second.price());
        if (price < mid_price_value) {
            continue;
        }
        const double distance = price - mid_price_value;
        if (distance > max_distance) {
            break;
        }
        const double price_weight = 1.0 - distance / max_distance;
        if (price_weight <= 0.0) {
            break;
        }
        const double weight = price_weight * static_cast<double>(it->second.total_volume());
        weighted_ask_sum += weight * price;
        ask_weight += weight;
    }

    const double effective_ask = ask_weight <= 1e-6 ? best_ask : weighted_ask_sum / ask_weight;
    const double effective_bid = bid_weight <= 1e-6 ? best_bid : weighted_bid_sum / bid_weight;
    const double hermes_price = (effective_ask + effective_bid) / 2.0;
    if (hermes_price <= 0.0) {
        return 0.0;
    }

    return clamp_double((hermes_price / mid_price_value - 1.0) * 1e3, -5.0, 5.0);
}

ShSzBookAggregateSummary ShSzFullOrderBookEngine::summarize_best_n_levels(bool is_sell,
                                                                          size_t max_levels,
                                                                          uint32_t now_time_ms) const {
    if (is_sell) {
        return summarize_levels(mAskBook.begin(), mAskBook.end(), max_levels, 0, 0, true, now_time_ms);
    }
    return summarize_levels(mBidBook.rbegin(), mBidBook.rend(), max_levels, 0, 0, false, now_time_ms);
}

ShSzBookAggregateSummary ShSzFullOrderBookEngine::summarize_levels_by_mid_bp(bool is_sell,
                                                                             int mid_price_value,
                                                                             int max_basis_points,
                                                                             uint32_t now_time_ms) const {
    if (mid_price_value <= 0 || max_basis_points <= 0) {
        return ShSzBookAggregateSummary();
    }
    if (is_sell) {
        return summarize_levels(mAskBook.begin(),
                                mAskBook.end(),
                                0,
                                mid_price_value,
                                max_basis_points,
                                true,
                                now_time_ms);
    }
    return summarize_levels(mBidBook.rbegin(),
                            mBidBook.rend(),
                            0,
                            mid_price_value,
                            max_basis_points,
                            false,
                            now_time_ms);
}

void ShSzFullOrderBookEngine::clear() {
    mAskBook.clear();
    mBidBook.clear();
    mOrderLocators.clear();
    mBidOrderCount = 0;
    mAskOrderCount = 0;
    mBidMaxLevelVolume = 0;
    mAskMaxLevelVolume = 0;
    mBidMaxLevelPrice = 0.0;
    mAskMaxLevelPrice = 0.0;
    mVisibleBookCache = ShSzVisibleBook();
    mBidLightSummary = ShSzBookAggregateSummary();
    mAskLightSummary = ShSzBookAggregateSummary();
}

uint32_t ShSzFullOrderBookEngine::parse_event_time_ms(const char* event_time) {
    return time_string_to_ms(event_time);
}

bool ShSzFullOrderBookEngine::reduce_order(long quote_tag, int volume, bool erase_when_flat) {
    std::unordered_map<long, OrderLocator>::iterator locator_it = mOrderLocators.find(quote_tag);
    if (locator_it == mOrderLocators.end()) {
        return false;
    }
    if (volume <= 0) {
        return false;
    }

    if (locator_it->second.is_sell) {
        AskBook::iterator level_it = mAskBook.find(locator_it->second.price);
        if (level_it == mAskBook.end()) {
            return false;
        }
        bool removed_order = false;
        if (!level_it->second.reduce_order(quote_tag, volume, erase_when_flat, &removed_order)) {
            return false;
        }
        if (removed_order) {
            mAskOrderCount -= 1;
            mOrderLocators.erase(locator_it);
        }
        if (level_it->second.empty()) {
            mAskBook.erase(level_it);
            if (mAskMaxLevelPrice == static_cast<double>(locator_it->second.price) / PRICE_MULTIPLIER) {
                rescan_ask_max_level();
            }
        } else {
            const int remaining_level_volume = level_it->second.total_volume();
            if (mAskMaxLevelPrice == static_cast<double>(locator_it->second.price) / PRICE_MULTIPLIER &&
                remaining_level_volume < mAskMaxLevelVolume) {
                rescan_ask_max_level();
            }
        }
        refresh_ask_side_cache(0);
        return true;
    }

    BidBook::iterator level_it = mBidBook.find(locator_it->second.price);
    if (level_it == mBidBook.end()) {
        return false;
    }
    bool removed_order = false;
    if (!level_it->second.reduce_order(quote_tag, volume, erase_when_flat, &removed_order)) {
        return false;
    }
    if (removed_order) {
        mBidOrderCount -= 1;
        mOrderLocators.erase(locator_it);
    }
    if (level_it->second.empty()) {
        mBidBook.erase(level_it);
        if (mBidMaxLevelPrice == static_cast<double>(locator_it->second.price) / PRICE_MULTIPLIER) {
            rescan_bid_max_level();
        }
    } else {
        const int remaining_level_volume = level_it->second.total_volume();
        if (mBidMaxLevelPrice == static_cast<double>(locator_it->second.price) / PRICE_MULTIPLIER &&
            remaining_level_volume < mBidMaxLevelVolume) {
            rescan_bid_max_level();
        }
    }
    refresh_bid_side_cache(0);
    return true;
}

bool ShSzFullOrderBookEngine::fill_visible_side(
    const std::map<int, ShSzFullOrderBookLevel>& book,
    bool is_bid,
    std::array<ShSzVisibleBookLevel, PRICE_LEVEL>* out,
    uint32_t now_time_ms) const {
    for (size_t i = 0; i < out->size(); ++i) {
        (*out)[i] = ShSzVisibleBookLevel();
    }

    size_t level_index = 0;
    if (is_bid) {
        for (std::map<int, ShSzFullOrderBookLevel>::const_reverse_iterator it = book.rbegin();
             it != book.rend() && level_index < out->size();
             ++it, ++level_index) {
            (*out)[level_index].price = it->second.price();
            (*out)[level_index].total_volume = it->second.total_volume();
            (*out)[level_index].order_count = it->second.order_count();
            (*out)[level_index].total_create_time_ms = it->second.total_create_time_ms();
            (*out)[level_index].window_volume = it->second.window_volume(now_time_ms);
            (*out)[level_index].valid = true;
        }
        return true;
    }

    for (std::map<int, ShSzFullOrderBookLevel>::const_iterator it = book.begin();
         it != book.end() && level_index < out->size();
         ++it, ++level_index) {
        (*out)[level_index].price = it->second.price();
        (*out)[level_index].total_volume = it->second.total_volume();
        (*out)[level_index].order_count = it->second.order_count();
        (*out)[level_index].total_create_time_ms = it->second.total_create_time_ms();
        (*out)[level_index].window_volume = it->second.window_volume(now_time_ms);
        (*out)[level_index].valid = true;
    }
    return true;
}

void ShSzFullOrderBookEngine::rescan_bid_max_level() {
    mBidMaxLevelVolume = 0;
    mBidMaxLevelPrice = 0.0;
    for (BidBook::const_iterator it = mBidBook.begin(); it != mBidBook.end(); ++it) {
        if (it->second.total_volume() > mBidMaxLevelVolume) {
            mBidMaxLevelVolume = it->second.total_volume();
            mBidMaxLevelPrice = static_cast<double>(it->second.price()) / PRICE_MULTIPLIER;
        }
    }
}

void ShSzFullOrderBookEngine::rescan_ask_max_level() {
    mAskMaxLevelVolume = 0;
    mAskMaxLevelPrice = 0.0;
    for (AskBook::const_iterator it = mAskBook.begin(); it != mAskBook.end(); ++it) {
        if (it->second.total_volume() > mAskMaxLevelVolume) {
            mAskMaxLevelVolume = it->second.total_volume();
            mAskMaxLevelPrice = static_cast<double>(it->second.price()) / PRICE_MULTIPLIER;
        }
    }
}

void ShSzFullOrderBookEngine::update_bid_max_level_on_add(int price, int level_volume) {
    if (level_volume > mBidMaxLevelVolume ||
        (level_volume == mBidMaxLevelVolume &&
         (mBidMaxLevelPrice <= 0.0 || price < static_cast<int>(mBidMaxLevelPrice * PRICE_MULTIPLIER)))) {
        mBidMaxLevelVolume = level_volume;
        mBidMaxLevelPrice = static_cast<double>(price) / PRICE_MULTIPLIER;
    }
}

void ShSzFullOrderBookEngine::update_ask_max_level_on_add(int price, int level_volume) {
    if (level_volume > mAskMaxLevelVolume ||
        (level_volume == mAskMaxLevelVolume &&
         (mAskMaxLevelPrice <= 0.0 || price < static_cast<int>(mAskMaxLevelPrice * PRICE_MULTIPLIER)))) {
        mAskMaxLevelVolume = level_volume;
        mAskMaxLevelPrice = static_cast<double>(price) / PRICE_MULTIPLIER;
    }
}

void ShSzFullOrderBookEngine::refresh_bid_side_cache(uint32_t now_time_ms) {
    for (size_t i = 0; i < mVisibleBookCache.bids.size(); ++i) {
        mVisibleBookCache.bids[i] = ShSzVisibleBookLevel();
    }

    size_t level_index = 0;
    for (BidBook::const_reverse_iterator it = mBidBook.rbegin();
         it != mBidBook.rend() && level_index < mVisibleBookCache.bids.size();
         ++it, ++level_index) {
        ShSzVisibleBookLevel& out = mVisibleBookCache.bids[level_index];
        out.price = it->second.price();
        out.total_volume = it->second.total_volume();
        out.order_count = it->second.order_count();
        out.total_create_time_ms = it->second.total_create_time_ms();
        out.window_volume = now_time_ms > 0 ? it->second.window_volume(now_time_ms) : 0;
        out.valid = true;
    }

    mBidLightSummary = ShSzBookAggregateSummary();
    if (!mBidBook.empty()) {
        const ShSzFullOrderBookLevel& best_level = mBidBook.rbegin()->second;
        mBidLightSummary.valid = true;
        mBidLightSummary.best_price = best_level.price();
        mBidLightSummary.best_volume = best_level.total_volume();
        mBidLightSummary.level_count = static_cast<int>(mBidBook.size());
        mBidLightSummary.total_order_count = mBidOrderCount;
        mBidLightSummary.max_level_volume = mBidMaxLevelVolume;
        mBidLightSummary.max_level_price = static_cast<int>(mBidMaxLevelPrice * PRICE_MULTIPLIER);
    }
}

void ShSzFullOrderBookEngine::refresh_ask_side_cache(uint32_t now_time_ms) {
    for (size_t i = 0; i < mVisibleBookCache.asks.size(); ++i) {
        mVisibleBookCache.asks[i] = ShSzVisibleBookLevel();
    }

    size_t level_index = 0;
    for (AskBook::const_iterator it = mAskBook.begin();
         it != mAskBook.end() && level_index < mVisibleBookCache.asks.size();
         ++it, ++level_index) {
        ShSzVisibleBookLevel& out = mVisibleBookCache.asks[level_index];
        out.price = it->second.price();
        out.total_volume = it->second.total_volume();
        out.order_count = it->second.order_count();
        out.total_create_time_ms = it->second.total_create_time_ms();
        out.window_volume = now_time_ms > 0 ? it->second.window_volume(now_time_ms) : 0;
        out.valid = true;
    }

    mAskLightSummary = ShSzBookAggregateSummary();
    if (!mAskBook.empty()) {
        const ShSzFullOrderBookLevel& best_level = mAskBook.begin()->second;
        mAskLightSummary.valid = true;
        mAskLightSummary.best_price = best_level.price();
        mAskLightSummary.best_volume = best_level.total_volume();
        mAskLightSummary.level_count = static_cast<int>(mAskBook.size());
        mAskLightSummary.total_order_count = mAskOrderCount;
        mAskLightSummary.max_level_volume = mAskMaxLevelVolume;
        mAskLightSummary.max_level_price = static_cast<int>(mAskMaxLevelPrice * PRICE_MULTIPLIER);
    }
}

void ShSzFullOrderBookEngine::refresh_side_cache(bool is_sell, uint32_t now_time_ms) {
    if (is_sell) {
        refresh_ask_side_cache(now_time_ms);
        return;
    }
    refresh_bid_side_cache(now_time_ms);
}

ShSzFullOrderBookSummary ShSzFullOrderBookEngine::snapshot_lightweight_summary() const {
    ShSzFullOrderBookSummary summary;
    summary.instrument_id_value = mInstrumentIdValue;
    summary.mid_price = mid_price();
    summary.bid = mBidLightSummary;
    summary.ask = mAskLightSummary;
    summary.total_order_count = mBidOrderCount + mAskOrderCount;
    return summary;
}

bool ShSzFullOrderBookEngine::refresh_visible_window_volume(
    const std::map<int, ShSzFullOrderBookLevel>& book,
    std::array<ShSzVisibleBookLevel, PRICE_LEVEL>* out,
    uint32_t now_time_ms) const {
    if (out == 0 || now_time_ms == 0) {
        return false;
    }

    for (size_t i = 0; i < out->size(); ++i) {
        if (!(*out)[i].valid) {
            continue;
        }
        std::map<int, ShSzFullOrderBookLevel>::const_iterator it = book.find((*out)[i].price);
        if (it == book.end()) {
            (*out)[i] = ShSzVisibleBookLevel();
            continue;
        }
        (*out)[i].window_volume = it->second.window_volume(now_time_ms);
    }
    return true;
}
