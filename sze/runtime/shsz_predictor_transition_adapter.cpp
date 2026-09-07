#include "sze/runtime/shsz_predictor_transition_adapter.h"

#include <cfloat>

#include "common/market_data/legacy/shsz_full_orderbook_diagnostics.h"

namespace {

inline double time_ms_to_market_time_value(uint32_t now_time_ms) {
    const uint32_t hours = now_time_ms / 3600000U;
    const uint32_t minutes = (now_time_ms % 3600000U) / 60000U;
    const uint32_t seconds = (now_time_ms % 60000U) / 1000U;
    const uint32_t milliseconds = now_time_ms % 1000U;
    return static_cast<double>(hours) * 10000000.0 +
           static_cast<double>(minutes) * 100000.0 +
           static_cast<double>(seconds) * 1000.0 +
           static_cast<double>(milliseconds);
}

inline void fill_snapshot_side(const std::array<ShSzVisibleBookLevel, PRICE_LEVEL>& levels,
                               bool is_bid,
                               MSMarketDataField* snapshot) {
    if (snapshot == 0) {
        return;
    }

    double* price_base = is_bid ? &snapshot->BidPrice1 : &snapshot->AskPrice1;
    double* volume_base = is_bid ? &snapshot->BidVolume1 : &snapshot->AskVolume1;
    for (int i = 0; i < PRICE_LEVEL; ++i) {
        if (levels[static_cast<size_t>(i)].valid) {
            price_base[i] = static_cast<double>(levels[static_cast<size_t>(i)].price) / PRICE_MULTIPLIER;
            volume_base[i] = static_cast<double>(levels[static_cast<size_t>(i)].total_volume);
        } else {
            price_base[i] = DBL_MAX;
            volume_base[i] = 0.0;
        }
    }
}

} // namespace

ShSzPredictorTransitionInput::ShSzPredictorTransitionInput()
    : mode(SHSZ_PREDICTOR_INPUT_INVALID),
      valid(false),
      has_signal_snapshot(false),
      signal_snapshot_storage(),
      full_orderbook_input() {
}

const MSMarketDataField* ShSzPredictorTransitionInput::signal_snapshot_ptr() const {
    return has_signal_snapshot
               ? reinterpret_cast<const MSMarketDataField*>(&signal_snapshot_storage)
               : 0;
}

bool ShSzPredictorTransitionInput::uses_full_orderbook() const {
    return mode == SHSZ_PREDICTOR_INPUT_FULL_ORDERBOOK;
}

bool ShSzPredictorTransitionInput::uses_legacy_snapshot() const {
    return mode == SHSZ_PREDICTOR_INPUT_LEGACY_SNAPSHOT;
}

ShSzPredictorTransitionInput ShSzPredictorTransitionAdapter::from_legacy_snapshot(const MSMarketDataField* snapshot) {
    ShSzPredictorTransitionInput input;
    if (snapshot == 0) {
        return input;
    }

    input.mode = SHSZ_PREDICTOR_INPUT_LEGACY_SNAPSHOT;
    input.valid = true;
    input.has_signal_snapshot = true;
    input.signal_snapshot_storage = snapshot->ms_market_data;
    return input;
}

ShSzPredictorTransitionInput ShSzPredictorTransitionAdapter::from_full_orderbook(
    const ShSzFullOrderBookManager& manager,
    const char* instrument_id,
    const ShSzOrderFlowSummary& order_flow,
    uint32_t now_time_ms,
    double fee_share,
    const MSMarketDataField* signal_seed) {
    ShSzPredictorTransitionInput input;
    const ShSzFullOrderBookEngine* engine = manager.get_engine(instrument_id);
    if (engine == 0) {
        return input;
    }

    const bool latency_enabled = shsz_full_orderbook_latency_enabled();
    const uint64_t factor_begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    input.full_orderbook_input =
        ShSzFullOrderBookFactorExtractor::build_predictor_input(manager, instrument_id, order_flow, now_time_ms, fee_share);
    if (latency_enabled) {
        ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
        stats.factor_count += 1;
        stats.factor_ns += (shsz_full_orderbook_now_ns() - factor_begin_ns);
    }
    if (!input.full_orderbook_input.valid) {
        return input;
    }

    input.mode = SHSZ_PREDICTOR_INPUT_FULL_ORDERBOOK;
    input.has_signal_snapshot = true;
    const uint64_t project_begin_ns = latency_enabled ? shsz_full_orderbook_now_ns() : 0;
    input.signal_snapshot_storage = project_signal_snapshot(*engine, now_time_ms, signal_seed);
    if (latency_enabled) {
        ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
        stats.project_snapshot_count += 1;
        stats.project_snapshot_ns += (shsz_full_orderbook_now_ns() - project_begin_ns);
    }
    input.valid = true;
    return input;
}

MSMarketData ShSzPredictorTransitionAdapter::project_signal_snapshot(const ShSzFullOrderBookEngine& engine,
                                                                     uint32_t now_time_ms,
                                                                     const MSMarketDataField* seed_snapshot) {
    MSMarketData snapshot_storage;
    if (seed_snapshot != 0) {
        snapshot_storage = seed_snapshot->ms_market_data;
    } else {
        snapshot_storage = MSMarketData();
    }
    MSMarketDataField* snapshot = reinterpret_cast<MSMarketDataField*>(&snapshot_storage);

    const ShSzVisibleBook visible_book = engine.snapshot_visible_book(now_time_ms);
    snapshot->InstrumentID = engine.instrument_id_value();
    snapshot->MarketTime = time_ms_to_market_time_value(now_time_ms);

    fill_snapshot_side(visible_book.bids, true, snapshot);
    fill_snapshot_side(visible_book.asks, false, snapshot);

    const int mid_price = engine.mid_price();
    if (mid_price > 0) {
        snapshot->MidPrice = static_cast<double>(mid_price) / PRICE_MULTIPLIER;
        if (snapshot->LastPrice == DBL_MAX || snapshot->LastPrice <= 0.0) {
            snapshot->LastPrice = snapshot->MidPrice;
        }
    } else if (snapshot->BidVolume1 > 0.0 && snapshot->AskVolume1 > 0.0 &&
               snapshot->BidPrice1 != DBL_MAX && snapshot->AskPrice1 != DBL_MAX) {
        snapshot->MidPrice = (snapshot->BidPrice1 + snapshot->AskPrice1) / 2.0;
        if (snapshot->LastPrice == DBL_MAX || snapshot->LastPrice <= 0.0) {
            snapshot->LastPrice = snapshot->MidPrice;
        }
    } else {
        snapshot->MidPrice = DBL_MAX;
    }

    snapshot->OrderPrice = DBL_MAX;
    snapshot->CancelPrice = DBL_MAX;
    snapshot->OrderVolume = 0.0;
    snapshot->CancelVolume = 0.0;
    snapshot->IsSell = 0.0;
    snapshot->IsCancel = 0.0;
    snapshot->AppSeq = 0.0;
    return snapshot_storage;
}
