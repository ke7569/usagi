#ifndef SSE_T0_TICK_PREDICTION_ENGINE_H
#define SSE_T0_TICK_PREDICTION_ENGINE_H

#include "../market_data/sse_tick_factors.h"
#include "../model/sse_model_runtime.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace sse_tick_strategy {

struct PredictionRow {
    std::string security_id;
    std::uint32_t channel_no;
    std::uint64_t tick_index;
    std::uint64_t exchange_time_micros;
    std::uint64_t factor_ns;
    std::uint64_t factor_l1_ns;
    std::uint64_t factor_flow_ns;
    std::uint64_t factor_depth_build_ns;
    std::uint64_t factor_depth_aggregate_ns;
    std::uint64_t factor_finalize_ns;
    std::uint64_t flow_event_count;
    std::uint64_t live_order_count;
    std::uint64_t bid_level_count;
    std::uint64_t ask_level_count;
    std::uint64_t infer_ns;
    std::array<float, sse_model::kFeatureCount> factors;
    float prediction;
    bool model_valid;
    bool factor_complete;
    bool selected;
    const char* quality;
    PredictionRow();
};

class PredictionEngine {
public:
    PredictionEngine();
    ~PredictionEngine();
    bool load(const std::string& model_path, std::string* error);
    bool set_free_share(const std::string& security_id, double free_share);
    bool set_static_metadata(const std::string& security_id,
                             const sse_tick::DailyStaticMetadata& metadata);
    bool observe(const sse_live::TickEvent& event,
                 std::uint64_t local_timestamp_micros,
                 std::uint64_t next_local_timestamp_micros,
                 PredictionRow* output,
                 std::string* error);
    // Update the latest Level2 cumulative fields used by tick factors.  This
    // never applies a book event and never advances the tick model state.
    bool observe_snapshot(const sse_live::Snapshot& snapshot,
                          std::string* error);
    // Evaluate a tick batch after the receiver's quiet-period marker.  This
    // is the only API that may advance the tick model's recurrent state.
    bool sample_tick(const sse_live::TickEvent& event,
                     std::uint64_t local_timestamp_micros,
                     std::uint64_t next_local_timestamp_micros,
                     PredictionRow* output,
                     std::string* error);
    bool sample_level2(const std::string& security_id,
                       std::uint32_t channel_no,
                       std::uint64_t exchange_time_micros,
                       std::uint64_t local_timestamp_micros,
                       std::uint64_t next_local_timestamp_micros,
                       const sse_live::Snapshot* snapshot,
                       PredictionRow* output,
                       std::string* error);
    void reset();

private:
    struct SecurityState {
        explicit SecurityState(const std::string& id)
            : book(id), factors(), model_state(), have_sample_reference(false),
              last_sample_turnover(0.0), last_sample_mid(0.0), last_sample_volume(0),
              have_snapshot_aux(false), snapshot_last_price(0.0),
              snapshot_volume(0.0), snapshot_turnover(0.0), snapshot_time_micros(0) {}
        sse_tick::OrderBook book;
        sse_tick::FactorState factors;
        sse_model::State model_state;
        bool have_sample_reference;
        double last_sample_turnover;
        double last_sample_mid;
        std::uint64_t last_sample_volume;
        bool have_snapshot_aux;
        double snapshot_last_price;
        double snapshot_volume;
        double snapshot_turnover;
        std::uint64_t snapshot_time_micros;
    };
    std::unordered_map<std::string, SecurityState*> states_;
    sse_model::Model model_;
    bool loaded_;
};

}  // namespace sse_tick_strategy

#endif  // SSE_T0_TICK_PREDICTION_ENGINE_H
