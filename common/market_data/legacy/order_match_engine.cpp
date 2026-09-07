#include "common/market_data/legacy/order_match_engine.h"
#include <iostream>
#include "common/market_data/legacy/snap_latency.h"

namespace {

inline bool is_continuous_trading_time(const char* quote_time) {
    return (strcmp(quote_time, "09:26:00.000") > 0) &&
           (strcmp(quote_time, "14:57:00.000") < 0);
}

}  // namespace

OrderMatchEngine::OrderMatchEngine()
    : mVolume(0), mTurnover(0.0), mLastPrice(DBL_MAX) {
    mQuoteTagPriceMap.reserve(32768);
    reset_snapshot_state();
}

void OrderMatchEngine::set_instrument_id_value(double instrument_id_value) {
    mInstrumentIdValue = instrument_id_value;
    mSnapshotState.ms_market_data[InstrumentIDIndex] = instrument_id_value;
}

void OrderMatchEngine::reset_snapshot_state() {
    mSnapshotState = MSMarketData();
    mSnapshotState.ms_market_data[InstrumentIDIndex] = mInstrumentIdValue;
}

void OrderMatchEngine::update_visible_side_from_book(bool is_bid) {
    const int price_index_base = is_bid ? BidPrice1Index : AskPrice1Index;
    const int volume_index_base = is_bid ? BidVolume1Index : AskVolume1Index;

    for (int i = 0; i < PRICE_LEVEL; ++i) {
        mSnapshotState.ms_market_data[price_index_base + i] = DBL_MAX;
        mSnapshotState.ms_market_data[volume_index_base + i] = 0.0;
    }

    if (is_bid) {
        int new_count = 0;
        for (auto it = mBidBook.rbegin(); it != mBidBook.rend() && new_count < PRICE_LEVEL; ++it, ++new_count) {
            const int volume = it->second;
            if (volume < 0) {
                throw std::runtime_error("bid volume cannot be negative");
            }
            mSnapshotState.ms_market_data[BidPrice1Index + new_count] =
                static_cast<double>(it->first) / PRICE_MULTIPLIER;
            mSnapshotState.ms_market_data[BidVolume1Index + new_count] =
                static_cast<double>(volume);
        }
    } else {
        int new_count = 0;
        for (auto it = mAskBook.begin(); it != mAskBook.end() && new_count < PRICE_LEVEL; ++it, ++new_count) {
            const int volume = it->second;
            if (volume < 0) {
                throw std::runtime_error("ask volume cannot be negative");
            }
            mSnapshotState.ms_market_data[AskPrice1Index + new_count] =
                static_cast<double>(it->first) / PRICE_MULTIPLIER;
            mSnapshotState.ms_market_data[AskVolume1Index + new_count] =
                static_cast<double>(volume);
        }
    }
}

void OrderMatchEngine::update_summary_fields() {
    mSnapshotState.ms_market_data[LastPriceIndex] = mLastPrice;
    mSnapshotState.ms_market_data[TurnoverIndex] = mTurnover;
    mSnapshotState.ms_market_data[VolumeIndex] = static_cast<double>(mVolume);

    if (mSnapshotState.ms_market_data[AskPrice1Index] == DBL_MAX ||
        mSnapshotState.ms_market_data[BidPrice1Index] == DBL_MAX) {
        mSnapshotState.ms_market_data[MidPriceIndex] = DBL_MAX;
    } else {
        mSnapshotState.ms_market_data[MidPriceIndex] =
            (mSnapshotState.ms_market_data[AskPrice1Index] +
             mSnapshotState.ms_market_data[BidPrice1Index]) / 2.0;
    }
}

void OrderMatchEngine::order_match(bool is_sell) {
    const bool latency_on = snap_latency_enabled();
    const uint64_t begin_ns = latency_on ? snap_now_ns() : 0;

    if (mBidBook.empty() || mAskBook.empty()) {
        if (latency_on) {
            auto& stats = snap_latency_stats();
            stats.engine_order_match_ns += (snap_now_ns() - begin_ns);
            ++stats.engine_order_match_count;
        }
        return;
    }

    while (mBidBook.rbegin()->first >= mAskBook.begin()->first) {
        auto bid_it = std::prev(mBidBook.end());
        auto ask_it = mAskBook.begin();
        int trade_qty = std::min(bid_it->second, ask_it->second);

        bid_it->second -= trade_qty;
        ask_it->second -= trade_qty;
        mVolume += trade_qty;

        if (is_sell) {
            mTurnover += trade_qty * static_cast<double>(bid_it->first) / PRICE_MULTIPLIER;
            mLastPrice = static_cast<double>(bid_it->first) / PRICE_MULTIPLIER;
        } else {
            mTurnover += trade_qty * static_cast<double>(ask_it->first) / PRICE_MULTIPLIER;
            mLastPrice = static_cast<double>(ask_it->first) / PRICE_MULTIPLIER;
        }

        if (bid_it->second == 0) {
            mBidBook.erase(bid_it);
        }
        if (ask_it->second == 0) {
            mAskBook.erase(ask_it);
        }
        if (mBidBook.empty() || mAskBook.empty()) {
            break;
        }
    }

    update_visible_side_from_book(true);
    update_visible_side_from_book(false);
    update_summary_fields();

    if (latency_on) {
        auto& stats = snap_latency_stats();
        stats.engine_order_match_ns += (snap_now_ns() - begin_ns);
        ++stats.engine_order_match_count;
    }
}

void OrderMatchEngine::insert_quote_data(MSQuoteData& quote_data) {
    const bool latency_on = snap_latency_enabled();
    const uint64_t begin_ns = latency_on ? snap_now_ns() : 0;

    const int volume = static_cast<int>(quote_data.Volume);
    const long quote_tag = quote_data.QuoteTag;
    int price = quote_data.Price;
    bool update_bid_side = false;
    bool update_ask_side = false;

    if (quote_data.is_sell && !quote_data.is_cancel && quote_data.Price == BEST_PRICE) {
        price = mAskBook.begin()->first;
    } else if (!quote_data.is_sell && !quote_data.is_cancel && quote_data.Price == BEST_PRICE) {
        price = mBidBook.rbegin()->first;
    }

    if (quote_data.is_sell && !quote_data.is_cancel) {
        auto book_it = mAskBook.find(price);
        if (book_it == mAskBook.end()) {
            book_it = mAskBook.emplace(price, 0).first;
        }
        mQuoteTagPriceMap[quote_tag] = price;
        book_it->second += volume;
        update_ask_side = true;
    }
    if (!quote_data.is_sell && !quote_data.is_cancel) {
        auto book_it = mBidBook.find(price);
        if (book_it == mBidBook.end()) {
            book_it = mBidBook.emplace(price, 0).first;
        }
        mQuoteTagPriceMap[quote_tag] = price;
        book_it->second += volume;
        update_bid_side = true;
    } else if (quote_data.is_sell && quote_data.is_cancel) {
        auto tag_it = mQuoteTagPriceMap.find(quote_tag);
        if (tag_it == mQuoteTagPriceMap.end()) {
            return;
        }
        price = tag_it->second;
        quote_data.Price = price;
        auto book_it = mAskBook.find(price);
        if (book_it == mAskBook.end()) {
            return;
        }
        book_it->second -= volume;
        if (book_it->second == 0) {
            mAskBook.erase(book_it);
        }
        update_ask_side = true;
    } else if (!quote_data.is_sell && quote_data.is_cancel) {
        auto tag_it = mQuoteTagPriceMap.find(quote_tag);
        if (tag_it == mQuoteTagPriceMap.end()) {
            return;
        }
        price = tag_it->second;
        quote_data.Price = price;
        auto book_it = mBidBook.find(price);
        if (book_it == mBidBook.end()) {
            return;
        }
        book_it->second -= volume;
        if (book_it->second == 0) {
            mBidBook.erase(book_it);
        }
        update_bid_side = true;
    }

    if (is_continuous_trading_time(quote_data.QuoteTime)) {
        const bool crossed = !mBidBook.empty() && !mAskBook.empty() &&
                             (mBidBook.rbegin()->first >= mAskBook.begin()->first);
        if (crossed) {
            order_match(quote_data.is_sell);
        } else {
            if (update_bid_side) {
                update_visible_side_from_book(true);
            }
            if (update_ask_side) {
                update_visible_side_from_book(false);
            }
            update_summary_fields();
        }
    } else {
        if (update_bid_side) {
            update_visible_side_from_book(true);
        }
        if (update_ask_side) {
            update_visible_side_from_book(false);
        }
        update_summary_fields();
    }

    if (latency_on) {
        auto& stats = snap_latency_stats();
        stats.engine_insert_quote_ns += (snap_now_ns() - begin_ns);
        ++stats.engine_insert_quote_count;
    }
}

void OrderMatchEngine::copy_snapshot_data(MSMarketData& market_data,
                                          const MSQuoteData& quote_data,
                                          double market_time_value) const {
    const bool latency_on = snap_latency_enabled();
    const uint64_t begin_ns = latency_on ? snap_now_ns() : 0;

    market_data = mSnapshotState;
    market_data.ms_market_data[MarketTimeIndex] = market_time_value;
    market_data.ms_market_data[IsSellIndex] = static_cast<double>(quote_data.is_sell);
    market_data.ms_market_data[IsCancelIndex] = static_cast<double>(quote_data.is_cancel);
    market_data.ms_market_data[AppSeqIndex] = static_cast<double>(quote_data.ApplSeqNum);

    if (quote_data.is_cancel) {
        market_data.ms_market_data[CancelVolumeIndex] = static_cast<double>(quote_data.Volume);
        market_data.ms_market_data[CancelPriceIndex] = static_cast<double>(quote_data.Price) / PRICE_MULTIPLIER;
        market_data.ms_market_data[OrderVolumeIndex] = 0.0;
        market_data.ms_market_data[OrderPriceIndex] = DBL_MAX;
    } else {
        market_data.ms_market_data[OrderVolumeIndex] = static_cast<double>(quote_data.Volume);
        market_data.ms_market_data[OrderPriceIndex] = static_cast<double>(quote_data.Price) / PRICE_MULTIPLIER;
        market_data.ms_market_data[CancelVolumeIndex] = 0.0;
        market_data.ms_market_data[CancelPriceIndex] = DBL_MAX;
    }

    if (latency_on) {
        auto& stats = snap_latency_stats();
        stats.engine_get_snap_ns += (snap_now_ns() - begin_ns);
        ++stats.engine_get_snap_count;
    }
}

void OrderMatchEngine::clear() {
    mBidBook.clear();
    mAskBook.clear();
    mQuoteTagPriceMap.clear();
    mVolume = 0;
    mTurnover = 0.0;
    mLastPrice = DBL_MAX;
    reset_snapshot_state();
}
