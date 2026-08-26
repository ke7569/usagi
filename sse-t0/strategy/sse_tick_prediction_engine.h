#ifndef SSE_T0_TICK_PREDICTION_ENGINE_H
#define SSE_T0_TICK_PREDICTION_ENGINE_H

#include "../market_data/sse_tick_factors.h"
#include "../model/sse_model_runtime.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sse_tick_strategy {

struct PredictionRow {
    std::string security_id;
    std::uint32_t channel_no;
    std::uint64_t tick_index;
    std::uint64_t exchange_time_micros;
    std::uint64_t factor_ns;
    std::uint64_t infer_ns;
    float prediction;
    bool model_valid;
    bool factor_complete;
    bool selected;
    const char* quality;
    PredictionRow();
};

struct Level2Request {
    std::string security_id;
    std::uint32_t channel_no;
    std::uint64_t exchange_time_micros;
    std::uint64_t local_timestamp_micros;
    std::uint64_t next_local_timestamp_micros;
    const sse_live::Snapshot* snapshot;
    Level2Request()
        : security_id(), channel_no(0U), exchange_time_micros(0U),
          local_timestamp_micros(0U), next_local_timestamp_micros(0U),
          snapshot(0) {}
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
    // Prepare all factor rows first, then run all model inferences.  This
    // keeps the model weights hot in cache across a batch while preserving
    // per-security recurrent state and timing fields.
    bool sample_level2_batch(const std::vector<Level2Request>& requests,
                             std::vector<PredictionRow>* outputs,
                             std::string* error);
    void reset();

private:
    struct SecurityState {
        explicit SecurityState(const std::string& id)
            : book(id), factors(), model_state(), have_sample_reference(false),
              last_sample_turnover(0.0), last_sample_mid(0.0), last_sample_volume(0) {}
        sse_tick::OrderBook book;
        sse_tick::FactorState factors;
        sse_model::State model_state;
        bool have_sample_reference;
        double last_sample_turnover;
        double last_sample_mid;
        std::uint64_t last_sample_volume;
    };
    std::unordered_map<std::string, SecurityState*> states_;
    sse_model::Model model_;
    bool loaded_;
};

}  // namespace sse_tick_strategy

#endif  // SSE_T0_TICK_PREDICTION_ENGINE_H
