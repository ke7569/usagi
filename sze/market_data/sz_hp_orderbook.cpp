#include "sze/market_data/sz_hp_orderbook.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace sz_hp {

namespace {

template <size_t N>
void copy_text(const char* source, std::array<char, N>* destination) {
    if (destination == 0) {
        return;
    }
    destination->fill('\0');
    if (source == 0) {
        return;
    }
    size_t length = 0;
    while (length + 1 < N && source[length] != '\0') {
        ++length;
    }
    std::memcpy(destination->data(), source, length);
}

bool parse_time_ms(const char* event_time, uint32_t* result) {
    if (event_time == 0 || result == 0 || event_time[0] == '\0') {
        return false;
    }

    size_t length = 0;
    while (length < 31 && event_time[length] != '\0') {
        ++length;
    }
    if (length == 31) {
        return false;
    }
    if (length < 8 || event_time[2] != ':' || event_time[5] != ':') {
        return false;
    }
    const auto digit = [](char value) { return value >= '0' && value <= '9'; };
    for (size_t i = 0; i < 8; ++i) {
        if (i == 2 || i == 5) {
            continue;
        }
        if (!digit(event_time[i])) {
            return false;
        }
    }

    const uint32_t hours = static_cast<uint32_t>((event_time[0] - '0') * 10 + event_time[1] - '0');
    const uint32_t minutes = static_cast<uint32_t>((event_time[3] - '0') * 10 + event_time[4] - '0');
    const uint32_t seconds = static_cast<uint32_t>((event_time[6] - '0') * 10 + event_time[7] - '0');
    if (hours >= 24 || minutes >= 60 || seconds >= 60) {
        return false;
    }

    uint32_t milliseconds = 0;
    if (length > 8) {
        if (event_time[8] != '.') {
            return false;
        }
        size_t fraction_length = length - 9;
        if (fraction_length == 0 || fraction_length > 3) {
            return false;
        }
        for (size_t i = 9; i < length; ++i) {
            if (!digit(event_time[i])) {
                return false;
            }
            milliseconds = milliseconds * 10U + static_cast<uint32_t>(event_time[i] - '0');
        }
        if (fraction_length == 1) {
            milliseconds *= 100U;
        } else if (fraction_length == 2) {
            milliseconds *= 10U;
        }
    }

    *result = ((hours * 60U + minutes) * 60U + seconds) * 1000U + milliseconds;
    return true;
}

void set_diagnostic(AdapterDiagnostic* diagnostic,
                    AdapterDiagnostic::Code code,
                    int64_t sequence,
                    const char* reason) {
    if (diagnostic == 0) {
        return;
    }
    diagnostic->code = code;
    diagnostic->sequence = sequence;
    diagnostic->reason = reason == 0 ? "unknown" : reason;
}

void add_level_sum(const Level& level, ShSzFullOrderSum* sum) {
    if (sum == 0) {
        return;
    }
    sum->volume_sum += static_cast<double>(level.volume());
    sum->count_sum += static_cast<int64_t>(level.order_count());
    sum->tsc_sum += static_cast<int64_t>(level.total_timestamp());
    sum->amt_sum += static_cast<double>(level.price()) * static_cast<double>(level.volume()) /
                    static_cast<double>(kPriceScale);
}

void add_level_prefix(const std::vector<Level>& levels,
                      size_t count,
                      ShSzFullOrderSum* sum) {
    const size_t limit = std::min(count, levels.size());
    for (size_t i = 0; i < limit; ++i) {
        add_level_sum(levels[i], sum);
    }
}

}  // namespace

uint32_t to_price(double price) {
    if (!(price > 0.0) || !std::isfinite(price)) {
        return 0;
    }
    return static_cast<uint32_t>((price + 0.0000005) * static_cast<double>(kPriceScale));
}

uint32_t parse_event_time_ms(const char* event_time) {
    uint32_t result = 0;
    return parse_time_ms(event_time, &result) ? result : 0;
}

AdapterDiagnostic::AdapterDiagnostic()
    : instrument(), event_index(0), code(kNone), sequence(0), bid_id(0), ask_id(0), reason() {
    instrument.fill('\0');
}

void AdapterDiagnostic::clear() {
    instrument.fill('\0');
    event_index = 0;
    code = kNone;
    sequence = 0;
    bid_id = 0;
    ask_id = 0;
    reason.clear();
}

OrderEvent::OrderEvent()
    : instrument(),
      sequence(0),
      business_index(0),
      order_id(0),
      event_time_ms(0),
      price(0),
      raw_price(0.0),
      quantity(0),
      is_buy(false),
      type(OrderType::kNoop),
      raw_order_type('\0') {
    instrument.fill('\0');
}

TradeEvent::TradeEvent()
    : instrument(),
      sequence(0),
      business_index(0),
      bid_id(0),
      ask_id(0),
      event_time_ms(0),
      price(0),
      raw_price(0.0),
      quantity(0),
      flag(TradeFlag::kNoop),
      raw_trade_flag('\0') {
    instrument.fill('\0');
}

MarketObservation::MarketObservation()
    : instrument(),
      event_time_ms(0),
      last_price(0.0),
      total_volume(0.0),
      turnover(0.0),
      upper_limit_price(0.0),
      lower_limit_price(0.0),
      bid_price(),
      ask_price(),
      bid_volume(),
      ask_volume(),
      valid(false) {
    instrument.fill('\0');
    bid_price.fill(0.0);
    ask_price.fill(0.0);
    bid_volume.fill(0.0);
    ask_volume.fill(0.0);
}

double MarketObservation::fast_mid_price() const {
    if (bid_volume[0] != 0.0 && ask_volume[0] != 0.0) {
        return (bid_price[0] + ask_price[0]) / 2.0;
    }
    return last_price;
}

bool MarketObservation::has_level_one() const {
    return bid_volume[0] != 0.0 && ask_volume[0] != 0.0;
}

bool EventAdapter::normalize_order(const LFL2OrderField& source,
                                   OrderEvent* destination,
                                   AdapterDiagnostic* diagnostic,
                                   uint64_t event_index) {
    if (diagnostic != 0) {
        diagnostic->clear();
        copy_text(source.InstrumentID, &diagnostic->instrument);
        diagnostic->event_index = event_index;
        diagnostic->sequence = source.ApplSeqNum;
    }
    if (destination == 0) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kNullInput, 0, "null order destination");
        return false;
    }
    *destination = OrderEvent();
    copy_text(source.InstrumentID, &destination->instrument);
    if (destination->instrument[0] == '\0') {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidInstrument,
                       source.ApplSeqNum, "empty instrument");
        return false;
    }
    if (source.ApplSeqNum <= 0) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidSequence,
                       source.ApplSeqNum, "invalid order sequence");
        return false;
    }
    uint32_t event_time_ms = 0;
    if (!parse_time_ms(source.OrderTime, &event_time_ms)) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidTime,
                       source.ApplSeqNum, "invalid order time");
        return false;
    }
    if (source.Volume <= 0.0 || !std::isfinite(source.Volume)) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidQuantity,
                       source.ApplSeqNum, "non-positive order quantity");
        return false;
    }

    destination->sequence = static_cast<uint64_t>(source.ApplSeqNum);
    destination->business_index = static_cast<uint64_t>(source.BizIndex);
    destination->order_id = static_cast<uint64_t>(source.ApplSeqNum);
    destination->event_time_ms = event_time_ms;
    destination->price = to_price(source.Price);
    destination->raw_price = source.Price;
    destination->quantity = static_cast<int64_t>(source.Volume);
    destination->is_buy = source.OrderKind[0] == 'B';
    destination->raw_order_type = source.OrdType[0];
    switch (source.OrdType[0]) {
        case 'U':
            destination->type = OrderType::kSelfBest;
            break;
        case '1':
            destination->type = OrderType::kMarketPrice;
            break;
        case '2':
            destination->type = OrderType::kLimitPrice;
            break;
        default:
            destination->type = OrderType::kNoop;
            break;
    }
    return true;
}

bool EventAdapter::normalize_trade(const LFL2TradeField& source,
                                   TradeEvent* destination,
                                   AdapterDiagnostic* diagnostic,
                                   uint64_t event_index) {
    if (diagnostic != 0) {
        diagnostic->clear();
        copy_text(source.InstrumentID, &diagnostic->instrument);
        diagnostic->event_index = event_index;
        diagnostic->sequence = source.ApplSeqNum;
        diagnostic->bid_id = source.BidApplSeqNum;
        diagnostic->ask_id = source.OfferApplSeqNum;
    }
    if (destination == 0) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kNullInput, 0, "null trade destination");
        return false;
    }
    *destination = TradeEvent();
    copy_text(source.InstrumentID, &destination->instrument);
    if (destination->instrument[0] == '\0') {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidInstrument,
                       source.ApplSeqNum, "empty instrument");
        return false;
    }
    if (source.ApplSeqNum <= 0) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidSequence,
                       source.ApplSeqNum, "invalid trade sequence");
        return false;
    }
    uint32_t event_time_ms = 0;
    if (!parse_time_ms(source.TradeTime, &event_time_ms)) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidTime,
                       source.ApplSeqNum, "invalid trade time");
        return false;
    }
    if (source.Volume <= 0.0 || !std::isfinite(source.Volume)) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidQuantity,
                       source.ApplSeqNum, "non-positive trade quantity");
        return false;
    }

    destination->sequence = static_cast<uint64_t>(source.ApplSeqNum);
    destination->business_index = static_cast<uint64_t>(source.BizIndex);
    destination->bid_id = static_cast<uint64_t>(source.BidApplSeqNum);
    destination->ask_id = static_cast<uint64_t>(source.OfferApplSeqNum);
    destination->event_time_ms = event_time_ms;
    destination->price = to_price(source.Price);
    destination->raw_price = source.Price;
    destination->quantity = static_cast<int64_t>(source.Volume);
    destination->raw_trade_flag = source.OrderKind[0];
    switch (source.OrderKind[0]) {
        case 'F':
            destination->flag = TradeFlag::kFill;
            break;
        case '4':
            destination->flag = TradeFlag::kCancel;
            break;
        default:
            destination->flag = TradeFlag::kNoop;
            break;
    }
    return true;
}

bool EventAdapter::normalize_observation(const LFL2MarketDataField& source,
                                         MarketObservation* destination,
                                         AdapterDiagnostic* diagnostic,
                                         uint64_t event_index) {
    if (diagnostic != 0) {
        diagnostic->clear();
        copy_text(source.InstrumentID, &diagnostic->instrument);
        diagnostic->event_index = event_index;
    }
    if (destination == 0) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kNullInput, 0, "null observation destination");
        return false;
    }
    *destination = MarketObservation();
    copy_text(source.InstrumentID, &destination->instrument);
    if (destination->instrument[0] == '\0') {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidInstrument, 0, "empty instrument");
        return false;
    }
    uint32_t event_time_ms = 0;
    if (!parse_time_ms(source.TimeStamp, &event_time_ms)) {
        set_diagnostic(diagnostic, AdapterDiagnostic::kInvalidTime, 0, "invalid observation time");
        return false;
    }
    destination->event_time_ms = event_time_ms;
    destination->last_price = source.LastPrice;
    destination->total_volume = source.TotalTradeVolume;
    destination->turnover = source.TotalTradeValue;
    destination->upper_limit_price = source.UpperLimitPrice;
    destination->lower_limit_price = source.LowerLimitPrice;
    destination->bid_price[0] = source.BidPrice1;
    destination->bid_price[1] = source.BidPrice2;
    destination->bid_price[2] = source.BidPrice3;
    destination->bid_price[3] = source.BidPrice4;
    destination->bid_price[4] = source.BidPrice5;
    destination->bid_price[5] = source.BidPrice6;
    destination->bid_price[6] = source.BidPrice7;
    destination->bid_price[7] = source.BidPrice8;
    destination->bid_price[8] = source.BidPrice9;
    destination->bid_price[9] = source.BidPriceA;
    destination->bid_volume[0] = source.BidVolume1;
    destination->bid_volume[1] = source.BidVolume2;
    destination->bid_volume[2] = source.BidVolume3;
    destination->bid_volume[3] = source.BidVolume4;
    destination->bid_volume[4] = source.BidVolume5;
    destination->bid_volume[5] = source.BidVolume6;
    destination->bid_volume[6] = source.BidVolume7;
    destination->bid_volume[7] = source.BidVolume8;
    destination->bid_volume[8] = source.BidVolume9;
    destination->bid_volume[9] = source.BidVolumeA;
    destination->ask_price[0] = source.OfferPrice1;
    destination->ask_price[1] = source.OfferPrice2;
    destination->ask_price[2] = source.OfferPrice3;
    destination->ask_price[3] = source.OfferPrice4;
    destination->ask_price[4] = source.OfferPrice5;
    destination->ask_price[5] = source.OfferPrice6;
    destination->ask_price[6] = source.OfferPrice7;
    destination->ask_price[7] = source.OfferPrice8;
    destination->ask_price[8] = source.OfferPrice9;
    destination->ask_price[9] = source.OfferPriceA;
    destination->ask_volume[0] = source.OfferVolume1;
    destination->ask_volume[1] = source.OfferVolume2;
    destination->ask_volume[2] = source.OfferVolume3;
    destination->ask_volume[3] = source.OfferVolume4;
    destination->ask_volume[4] = source.OfferVolume5;
    destination->ask_volume[5] = source.OfferVolume6;
    destination->ask_volume[6] = source.OfferVolume7;
    destination->ask_volume[7] = source.OfferVolume8;
    destination->ask_volume[8] = source.OfferVolume9;
    destination->ask_volume[9] = source.OfferVolumeA;
    destination->valid = destination->has_level_one();
    return true;
}

SlidingWindow::SlidingWindow() : ring_(), total_(0), now_sec_(0) {
    clear();
}

bool SlidingWindow::add(uint32_t event_time_ms, Volume volume) {
    if (volume == 0) {
        return true;
    }
    const uint32_t tick = event_time_ms / 1000U;
    advance_to(tick);
    if (!in_window(tick)) {
        return true;
    }
    Bucket& target = bucket(tick);
    if (target.tick != tick) {
        total_ -= target.sum;
        target.tick = tick;
        target.sum = 0;
    }
    target.sum += volume;
    total_ += volume;
    return true;
}

bool SlidingWindow::erase(uint32_t event_time_ms, Volume volume) {
    if (volume == 0) {
        return true;
    }
    const uint32_t tick = event_time_ms / 1000U;
    if (tick > now_sec_) {
        advance_to(tick);
    }
    if (!in_window(tick)) {
        return true;
    }
    Bucket& target = bucket(tick);
    if (target.tick != tick) {
        return true;
    }
    if (target.sum < volume) {
        return false;
    }
    target.sum -= volume;
    total_ -= volume;
    return true;
}

SlidingWindow::Volume SlidingWindow::total(uint32_t now_time_ms) const {
    advance_to(now_time_ms / 1000U);
    return total_;
}

void SlidingWindow::clear() {
    for (size_t i = 0; i < ring_.size(); ++i) {
        ring_[i].tick = 0;
        ring_[i].sum = 0;
    }
    total_ = 0;
    now_sec_ = 0;
}

uint32_t SlidingWindow::now_sec() const {
    return now_sec_;
}

SlidingWindow::Bucket& SlidingWindow::bucket(uint32_t tick) const {
    return ring_[tick % kWindowRingSize];
}

bool SlidingWindow::in_window(uint32_t tick) const {
    return tick <= now_sec_ && (now_sec_ - tick) <= kWindowSec;
}

void SlidingWindow::advance_to(uint32_t tick) const {
    if (tick <= now_sec_) {
        return;
    }
    const uint32_t old_tick = now_sec_;
    now_sec_ = tick;
    const uint32_t gap = tick - old_tick;
    if (gap >= kWindowRingSize) {
        for (size_t i = 0; i < ring_.size(); ++i) {
            ring_[i].tick = 0;
            ring_[i].sum = 0;
        }
        total_ = 0;
        return;
    }
    for (uint32_t current = old_tick + 1; current <= tick; ++current) {
        const uint32_t expired_tick = current - static_cast<uint32_t>(kWindowRingSize);
        Bucket& expired = bucket(expired_tick);
        if (expired.tick == expired_tick) {
            total_ -= expired.sum;
            expired.tick = 0;
            expired.sum = 0;
        }
    }
}

QuoteOrder::QuoteOrder()
    : volume(0), trade_quantity(0), timestamp(0), is_buy(false) {
}

QuoteOrder::QuoteOrder(int64_t quantity, uint64_t timestamp_ms, bool buy)
    : volume(quantity), trade_quantity(0), timestamp(timestamp_ms), is_buy(buy) {
}

int64_t QuoteOrder::left() const {
    return volume - trade_quantity;
}

bool QuoteOrder::on_trade(int64_t quantity) {
    trade_quantity += quantity;
    return trade_quantity >= volume;
}

Level::Level(uint32_t price)
    : price_(price), volume_(0), total_timestamp_(0), orders_(), window_() {
    orders_.reserve(512);
}

bool Level::add_order(uint64_t order_id,
                      int64_t quantity,
                      uint64_t timestamp_ms,
                      bool is_buy) {
    const std::pair<std::unordered_map<uint64_t, QuoteOrder>::iterator, bool> inserted =
        orders_.emplace(order_id, QuoteOrder(quantity, timestamp_ms, is_buy));
    if (!inserted.second) {
        return false;
    }
    volume_ += quantity;
    total_timestamp_ += timestamp_ms;
    return window_.add(static_cast<uint32_t>(timestamp_ms), quantity);
}

bool Level::on_trade(uint64_t order_id, int64_t quantity) {
    std::unordered_map<uint64_t, QuoteOrder>::iterator it = orders_.find(order_id);
    if (it == orders_.end()) {
        return false;
    }
    volume_ -= quantity;
    const uint64_t timestamp = it->second.timestamp;
    if (it->second.on_trade(quantity)) {
        total_timestamp_ -= timestamp;
        orders_.erase(it);
    }
    return window_.erase(static_cast<uint32_t>(timestamp), quantity);
}

bool Level::cancel_order(uint64_t order_id) {
    std::unordered_map<uint64_t, QuoteOrder>::iterator it = orders_.find(order_id);
    if (it == orders_.end()) {
        return false;
    }
    const int64_t left_quantity = it->second.left();
    volume_ -= left_quantity;
    total_timestamp_ -= it->second.timestamp;
    const bool success = window_.erase(static_cast<uint32_t>(it->second.timestamp), left_quantity);
    orders_.erase(it);
    return success;
}

uint32_t Level::price() const {
    return price_;
}

int64_t Level::volume() const {
    return volume_;
}

uint64_t Level::total_timestamp() const {
    return total_timestamp_;
}

size_t Level::order_count() const {
    return orders_.size();
}

int64_t Level::window_volume_now() const {
    return window_.total(window_.now_sec() * 1000U);
}

bool Level::empty() const {
    return orders_.empty();
}

int64_t Level::window_volume(uint32_t now_time_ms) const {
    return window_.total(now_time_ms);
}

const std::unordered_map<uint64_t, QuoteOrder>& Level::orders() const {
    return orders_;
}

Depth::Depth(bool buy_side) : buy_side_(buy_side), levels_() {
    levels_.reserve(512);
}

size_t Depth::lower_bound_index(uint32_t price) const {
    size_t first = 0;
    size_t last = levels_.size();
    while (first < last) {
        const size_t middle = first + (last - first) / 2;
        const uint32_t middle_price = levels_[middle].price();
        const bool comes_before = buy_side_ ? (middle_price > price) : (middle_price < price);
        if (comes_before) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    return first;
}

Level* Depth::find(uint32_t price) {
    const size_t index = lower_bound_index(price);
    return index < levels_.size() && levels_[index].price() == price ? &levels_[index] : 0;
}

const Level* Depth::find(uint32_t price) const {
    const size_t index = lower_bound_index(price);
    return index < levels_.size() && levels_[index].price() == price ? &levels_[index] : 0;
}

Level& Depth::find_or_create(uint32_t price) {
    const size_t index = lower_bound_index(price);
    if (index < levels_.size() && levels_[index].price() == price) {
        return levels_[index];
    }
    levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(index), Level(price));
    return levels_[index];
}

bool Depth::erase(uint32_t price) {
    const size_t index = lower_bound_index(price);
    if (index >= levels_.size() || levels_[index].price() != price) {
        return false;
    }
    levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

size_t Depth::count() const {
    return levels_.size();
}

Level* Depth::first_level() {
    return levels_.empty() ? 0 : &levels_.front();
}

const Level* Depth::first_level() const {
    return levels_.empty() ? 0 : &levels_.front();
}

Level* Depth::last_level() {
    return levels_.empty() ? 0 : &levels_.back();
}

const Level* Depth::last_level() const {
    return levels_.empty() ? 0 : &levels_.back();
}

const std::vector<Level>& Depth::levels() const {
    return levels_;
}

void Depth::clear() {
    levels_.clear();
}

OrderBook::OrderBook(const std::string& instrument)
    : instrument_(instrument),
      bids_(true),
      asks_(false),
      available_(true),
      failure_sequence_(0),
      failure_reason_(),
      last_event_time_ms_(0) {
}

bool OrderBook::update_order(const OrderEvent& event) {
    if (!available_ || event.type == OrderType::kNoop) {
        return true;
    }
    last_event_time_ms_ = event.event_time_ms;
    if (event.quantity <= 0 || event.order_id == 0) {
        return fail(event.sequence, "invalid order");
    }
    if (event.type == OrderType::kSelfBest) {
        Depth* depth = event.is_buy ? &bids_ : &asks_;
        Level* level = depth->first_level();
        if (level == 0) {
            level = &depth->find_or_create(event.price);
        }
        if (!level->add_order(event.order_id,
                              event.quantity,
                              event.event_time_ms,
                              event.is_buy)) {
            return fail(event.sequence, "self-best order insert failed");
        }
        return true;
    }
    return add_order(event.order_id,
                     event.is_buy,
                     event.price,
                     event.quantity,
                     event.event_time_ms,
                     event.type);
}

bool OrderBook::update_trade(const TradeEvent& event) {
    if (!available_ || event.flag == TradeFlag::kNoop) {
        return true;
    }
    last_event_time_ms_ = event.event_time_ms;
    if (event.quantity <= 0) {
        return fail(event.sequence, "invalid trade quantity");
    }
    if (event.flag == TradeFlag::kCancel) {
        const uint64_t order_id = std::max(event.bid_id, event.ask_id);
        const bool is_buy = event.bid_id > event.ask_id;
        return cancel_order(order_id,
                            event.price,
                            is_buy,
                            event.sequence,
                            event.event_time_ms);
    }

    Level* bid_level = bids_.first_level();
    Level* ask_level = asks_.first_level();
    if (bid_level == 0 || ask_level == 0) {
        return fail(event.sequence, "level one missing for fill");
    }
    if (!bid_level->on_trade(event.bid_id, event.quantity)) {
        return fail(event.sequence, "bid level one fill failed");
    }
    if (bid_level->empty()) {
        bids_.erase(bid_level->price());
    }
    ask_level = asks_.first_level();
    if (ask_level == 0 || !ask_level->on_trade(event.ask_id, event.quantity)) {
        return fail(event.sequence, "ask level one fill failed");
    }
    if (ask_level->empty()) {
        asks_.erase(ask_level->price());
    }
    return true;
}

void OrderBook::reject(uint64_t sequence, const char* reason) {
    fail(sequence, reason);
}

bool OrderBook::add_order(uint64_t order_id,
                          bool is_buy,
                          uint32_t price,
                          int64_t quantity,
                          uint64_t timestamp_ms,
                          OrderType type) {
    if (!available_) {
        return true;
    }
    last_event_time_ms_ = static_cast<uint32_t>(timestamp_ms);
    if (order_id == 0 || quantity <= 0 ||
        (price == 0 && type != OrderType::kSelfBest && type != OrderType::kMarketPrice)) {
        return fail(order_id, "invalid limit order");
    }
    Depth* depth = is_buy ? &bids_ : &asks_;
    Level* level = type == OrderType::kSelfBest ? depth->first_level() : 0;
    if (level == 0) {
        level = &depth->find_or_create(price);
    }
    if (!level->add_order(order_id, quantity, timestamp_ms, is_buy)) {
        return fail(order_id, "duplicate order id");
    }
    return true;
}

bool OrderBook::cancel_order(uint64_t order_id,
                             uint32_t price,
                             bool is_buy,
                             uint64_t sequence,
                             uint64_t timestamp_ms) {
    (void)timestamp_ms;
    if (!available_) {
        return true;
    }
    last_event_time_ms_ = static_cast<uint32_t>(timestamp_ms);
    if (order_id == 0) {
        return fail(sequence, "invalid cancel order id");
    }
    Depth* depth = is_buy ? &bids_ : &asks_;
    Level* target = depth->find(price);
    if (target != 0 && target->cancel_order(order_id)) {
        if (target->empty()) {
            depth->erase(target->price());
        }
        return true;
    }

    // MatchBook<SZ> uses an ordered fallback scan after the feed price lookup.
    const std::vector<Level>& levels = depth->levels();
    for (size_t i = 0; i < levels.size(); ++i) {
        Level* candidate = depth->find(levels[i].price());
        if (candidate == 0 || candidate->volume() == 0 || candidate->empty()) {
            continue;
        }
        if (candidate->cancel_order(order_id)) {
            const uint32_t candidate_price = candidate->price();
            if (candidate->empty()) {
                depth->erase(candidate_price);
            }
            return true;
        }
    }
    return fail(sequence, "cancel order not found");
}

const std::string& OrderBook::instrument() const {
    return instrument_;
}

const Depth& OrderBook::bids() const {
    return bids_;
}

const Depth& OrderBook::asks() const {
    return asks_;
}

bool OrderBook::available() const {
    return available_;
}

uint64_t OrderBook::failure_sequence() const {
    return failure_sequence_;
}

const std::string& OrderBook::failure_reason() const {
    return failure_reason_;
}

size_t OrderBook::order_count() const {
    size_t count = 0;
    const std::vector<Level>& bid_levels = bids_.levels();
    const std::vector<Level>& ask_levels = asks_.levels();
    for (size_t i = 0; i < bid_levels.size(); ++i) {
        count += bid_levels[i].order_count();
    }
    for (size_t i = 0; i < ask_levels.size(); ++i) {
        count += ask_levels[i].order_count();
    }
    return count;
}

int64_t OrderBook::best_bid_volume() const {
    const Level* level = bids_.first_level();
    return level == 0 ? 0 : level->volume();
}

int64_t OrderBook::best_ask_volume() const {
    const Level* level = asks_.first_level();
    return level == 0 ? 0 : level->volume();
}

uint32_t OrderBook::best_bid_price() const {
    const Level* level = bids_.first_level();
    return level == 0 ? 0 : level->price();
}

uint32_t OrderBook::best_ask_price() const {
    const Level* level = asks_.first_level();
    return level == 0 ? 0 : level->price();
}

uint32_t OrderBook::mid_price() const {
    if (best_bid_price() == 0 || best_ask_price() == 0) {
        return 0;
    }
    return (best_bid_price() + best_ask_price()) / 2U;
}

ShSzFullOb OrderBook::snapshot_full_orderbook_aggregate(uint32_t now_time_ms) const {
    (void)now_time_ms;
    ShSzFullOb full_ob;
    full_ob.ask_total_count = static_cast<int64_t>(asks_.count());
    full_ob.bid_total_count = static_cast<int64_t>(bids_.count());

    const std::vector<Level>& ask_levels = asks_.levels();
    const std::vector<Level>& bid_levels = bids_.levels();
    for (size_t i = 0; i < ask_levels.size(); ++i) {
        if (ask_levels[i].volume() > full_ob.ask_max_volume) {
            full_ob.ask_max_volume = ask_levels[i].volume();
            full_ob.ask_max_level_price = static_cast<double>(ask_levels[i].price()) /
                                          static_cast<double>(kPriceScale);
        }
    }
    for (size_t i = 0; i < bid_levels.size(); ++i) {
        if (bid_levels[i].volume() > full_ob.bid_max_volume) {
            full_ob.bid_max_volume = bid_levels[i].volume();
            full_ob.bid_max_level_price = static_cast<double>(bid_levels[i].price()) /
                                          static_cast<double>(kPriceScale);
        }
    }
    if (ask_levels.empty() || bid_levels.empty()) {
        return full_ob;
    }

    full_ob.valid = true;
    full_ob.mp = (static_cast<double>(ask_levels.front().price()) +
                  static_cast<double>(bid_levels.front().price())) /
                 (2.0 * static_cast<double>(kPriceScale));
    full_ob.ask_level1.price = ask_levels.front().price();
    full_ob.bid_level1.price = bid_levels.front().price();
    add_level_sum(ask_levels.front(), &full_ob.ask_level1);
    add_level_sum(bid_levels.front(), &full_ob.bid_level1);
    add_level_prefix(ask_levels, 5, &full_ob.ask_level5);
    add_level_prefix(bid_levels, 5, &full_ob.bid_level5);

    for (size_t i = 0; i < ask_levels.size(); ++i) {
        const double price = static_cast<double>(ask_levels[i].price()) /
                             static_cast<double>(kPriceScale);
        bool continue_scan = false;
        if (price < full_ob.mp * 1.01) {
            add_level_sum(ask_levels[i], &full_ob.ask_01);
        }
        if (price < full_ob.mp * 1.05) {
            add_level_sum(ask_levels[i], &full_ob.ask_05);
        }
        if (price < full_ob.mp * 1.10) {
            add_level_sum(ask_levels[i], &full_ob.ask_10);
            continue_scan = true;
        }
        if (i < 5) {
            continue_scan = true;
        }
        if (!continue_scan) {
            break;
        }
    }
    for (size_t i = 0; i < bid_levels.size(); ++i) {
        const double price = static_cast<double>(bid_levels[i].price()) /
                             static_cast<double>(kPriceScale);
        bool continue_scan = false;
        if (price > full_ob.mp * 0.99) {
            add_level_sum(bid_levels[i], &full_ob.bid_01);
        }
        if (price > full_ob.mp * 0.95) {
            add_level_sum(bid_levels[i], &full_ob.bid_05);
        }
        if (price > full_ob.mp * 0.90) {
            add_level_sum(bid_levels[i], &full_ob.bid_10);
            continue_scan = true;
        }
        if (i < 5) {
            continue_scan = true;
        }
        if (!continue_scan) {
            break;
        }
    }
    return full_ob;
}

double OrderBook::young_orderbook_imbalance(int max_basis_points,
                                            uint32_t now_time_ms) const {
    if (max_basis_points <= 0 || bids_.count() == 0 || asks_.count() == 0) {
        return 0.0;
    }
    const double mid = static_cast<double>(mid_price()) / static_cast<double>(kPriceScale);
    const double max_distance = mid * static_cast<double>(max_basis_points) / 10000.0;
    if (mid <= 0.0 || max_distance <= 0.0) {
        return 0.0;
    }
    double bid_sum = 0.0;
    double ask_sum = 0.0;
    const uint32_t mid_key = to_price(mid);
    const std::vector<Level>& bid_levels = bids_.levels();
    for (size_t i = 0; i < bid_levels.size(); ++i) {
        if (bid_levels[i].price() >= mid_key) {
            continue;
        }
        const double distance = static_cast<double>(mid_key - bid_levels[i].price()) /
                                static_cast<double>(kPriceScale);
        if (distance > max_distance) {
            break;
        }
        bid_sum += static_cast<double>(bid_levels[i].window_volume(now_time_ms)) *
                   (1.0 - distance / max_distance);
    }
    const std::vector<Level>& ask_levels = asks_.levels();
    for (size_t i = 0; i < ask_levels.size(); ++i) {
        if (ask_levels[i].price() <= mid_key) {
            continue;
        }
        const double distance = static_cast<double>(ask_levels[i].price() - mid_key) /
                                static_cast<double>(kPriceScale);
        if (distance > max_distance) {
            break;
        }
        ask_sum += static_cast<double>(ask_levels[i].window_volume(now_time_ms)) *
                   (1.0 - distance / max_distance);
    }
    if (ask_sum <= 1e-6 && bid_sum <= 1e-6) {
        return 0.0;
    }
    return (ask_sum - bid_sum) / (ask_sum + bid_sum);
}

double OrderBook::fix_dist_hermes(int max_basis_points, uint32_t now_time_ms) const {
    (void)now_time_ms;
    if (max_basis_points <= 0 || bids_.count() == 0 || asks_.count() == 0) {
        return 0.0;
    }
    const uint32_t mid_key = mid_price();
    const double max_distance = static_cast<double>(mid_key) *
                                static_cast<double>(max_basis_points) / 10000.0;
    if (mid_key == 0 || max_distance <= 0.0) {
        return 0.0;
    }
    double weighted_bid = 0.0;
    double bid_weight = 0.0;
    const std::vector<Level>& bid_levels = bids_.levels();
    for (size_t i = 0; i < bid_levels.size(); ++i) {
        if (bid_levels[i].price() > mid_key) {
            continue;
        }
        const double distance = static_cast<double>(mid_key - bid_levels[i].price());
        if (distance > max_distance) {
            break;
        }
        const double price_weight = 1.0 - distance / max_distance;
        if (price_weight <= 0.0) {
            break;
        }
        const double weight = price_weight * static_cast<double>(bid_levels[i].volume());
        weighted_bid += weight * static_cast<double>(bid_levels[i].price());
        bid_weight += weight;
    }
    double weighted_ask = 0.0;
    double ask_weight = 0.0;
    const std::vector<Level>& ask_levels = asks_.levels();
    for (size_t i = 0; i < ask_levels.size(); ++i) {
        if (ask_levels[i].price() < mid_key) {
            continue;
        }
        const double distance = static_cast<double>(ask_levels[i].price() - mid_key);
        if (distance > max_distance) {
            break;
        }
        const double price_weight = 1.0 - distance / max_distance;
        if (price_weight <= 0.0) {
            break;
        }
        const double weight = price_weight * static_cast<double>(ask_levels[i].volume());
        weighted_ask += weight * static_cast<double>(ask_levels[i].price());
        ask_weight += weight;
    }
    const double best_ask = static_cast<double>(best_ask_price());
    const Level* fallback_bid_level = bids_.last_level();
    const double best_bid = fallback_bid_level == 0
                                ? 0.0
                                : static_cast<double>(fallback_bid_level->price());
    const double effective_ask = ask_weight <= 1e-6 ? best_ask : weighted_ask / ask_weight;
    const double effective_bid = bid_weight <= 1e-6 ? best_bid : weighted_bid / bid_weight;
    const double hermes_price = (effective_ask + effective_bid) / 2.0;
    if (hermes_price <= 0.0) {
        return 0.0;
    }
    const double result = (hermes_price / static_cast<double>(mid_key) - 1.0) * 1e3;
    return std::max(-5.0, std::min(5.0, result));
}

std::string OrderBook::digest() const {
    std::ostringstream out;
    out << "available=" << (available_ ? 1 : 0)
        << ";failure_seq=" << failure_sequence_
        << ";failure_reason=" << failure_reason_
        << ";event_time_ms=" << last_event_time_ms_
        << ";best_bid=" << best_bid_price()
        << ";best_ask=" << best_ask_price()
        << ";levels=" << bids_.count() << "," << asks_.count()
        << ";orders=" << order_count();

    const auto append_depth = [&out, this](const char* name, const Depth& depth) {
        out << ";" << name << "=";
        const std::vector<Level>& levels = depth.levels();
        for (size_t i = 0; i < levels.size(); ++i) {
            const Level& level = levels[i];
            out << level.price() << ":" << level.volume() << ":" << level.total_timestamp()
                << ":" << level.order_count() << ":"
                << level.window_volume(last_event_time_ms_) << "[";
            std::vector<uint64_t> ids;
            ids.reserve(level.orders().size());
            for (std::unordered_map<uint64_t, QuoteOrder>::const_iterator it = level.orders().begin();
                 it != level.orders().end(); ++it) {
                ids.push_back(it->first);
            }
            std::sort(ids.begin(), ids.end());
            for (size_t j = 0; j < ids.size(); ++j) {
                const QuoteOrder& order = level.orders().find(ids[j])->second;
                if (j != 0) {
                    out << ",";
                }
                out << ids[j] << "/" << order.left() << "/" << order.timestamp;
            }
            out << "]";
        }
    };
    append_depth("bid", bids_);
    append_depth("ask", asks_);
    return out.str();
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    available_ = true;
    failure_sequence_ = 0;
    failure_reason_.clear();
    last_event_time_ms_ = 0;
}

bool OrderBook::fill_side(Depth* depth,
                          uint64_t order_id,
                          int64_t quantity,
                          uint64_t sequence,
                          bool buy_side) {
    if (depth == 0) {
        return fail(sequence, buy_side ? "missing bid depth" : "missing ask depth");
    }
    Level* level = depth->first_level();
    if (level == 0 || !level->on_trade(order_id, quantity)) {
        return fail(sequence, buy_side ? "bid level one fill failed" : "ask level one fill failed");
    }
    if (level->empty()) {
        depth->erase(level->price());
    }
    return true;
}

bool OrderBook::fail(uint64_t sequence, const char* reason) {
    if (available_) {
        available_ = false;
        failure_sequence_ = sequence;
        failure_reason_ = reason == 0 ? "book update failed" : reason;
    }
    return false;
}

}  // namespace sz_hp
