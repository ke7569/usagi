#ifndef SSE_MODEL_CONSUMER_H
#define SSE_MODEL_CONSUMER_H
#include "../market_data/sse_event.h"
#include "../market_data/sse_latency_histogram.h"
#include "../market_data/sse_tick_static_metadata.h"
#include "../model/auction59_sidecar.h"
#include "../model/snapshot_ensemble.h"
#include "sse_tick_prediction_engine.h"
#include <map>
#include <memory>

namespace sse_pipeline {
struct ModelOptions {
    std::string tick_model, static_json;
    std::string snapshot_baseline, snapshot_baseline_scaler, snapshot_auction, snapshot_auction_scaler, auction_csv;
    std::uint32_t trading_day;
    std::uint64_t tick_start_us;
    bool snapshot_enabled;
    ModelOptions() : trading_day(0), tick_start_us(33300000000ULL), snapshot_enabled(false) {}
};

struct ModelStats {
    std::uint64_t ticks=0, snapshots=0, samples=0, predictions=0, selected=0;
    std::uint64_t tick_rejected=0, model_errors=0, snapshot_predictions=0, snapshot_rejected=0;
    std::uint64_t missing_metadata=0, missing_auction=0;
    double prediction_sum=0.0;
    std::map<std::string, std::uint64_t> quality;
    LatencyHistogram observe, factor, infer, snapshot_infer;
};

// One instance and one recurrent state per stock, owned by its shard thread.
// No stream formatting or disk I/O after load(). Snapshot and tick models have
// independent state and use exchange time for the 09:35 source selection.
class ModelConsumer {
public:
    explicit ModelConsumer(const ModelOptions& options);
    bool load(std::string* error);
    bool consume(const Event& event);
    const ModelStats& stats() const { return stats_; }
    const std::map<std::string, std::uint64_t>& stock_predictions() const { return stock_predictions_; }
    const std::string& error() const { return error_; }
private:
    struct SnapshotState {
        bool previous_valid=false;
        sse_live::Snapshot previous;
        sse_snapshot_gru::DualState model;
    };
    ModelOptions options_;
    sse_tick_strategy::PredictionEngine tick_;
    sse_tick::DailyStaticMetadataMap metadata_;
    sse_auction59::FactorMap auction_;
    sse_snapshot_gru::Ensemble snapshot_;
    std::map<std::string, bool> initialized_;
    std::map<std::string, SnapshotState> snapshots_;
    std::map<std::string, std::uint64_t> stock_predictions_;
    ModelStats stats_;
    std::string error_;
};
}
#endif
