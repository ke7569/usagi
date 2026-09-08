#ifndef SSE_COMPUTE_PIPELINE_H
#define SSE_COMPUTE_PIPELINE_H

#include "sse/runtime/sse_stream_processor.h"

namespace sse_stream {

// The frontend owns clocks, wire continuity, routing and output callbacks.
// Workers never retain borrowed UDP bytes or read another worker's book.
class ComputePipeline {
public:
    ComputePipeline(const sse_tick::DailyStaticMetadataMap& metadata,
                    const sse_hybrid_model::Model* model, bool factors_only,
                    const OutputCallback& callback,
                    const Auction59Provider& auction59_provider,
                    const PipelineConfig& config);
    ~ComputePipeline();
    void on_event(const deepwin_market_data::StreamEvent& event);
    void poll_outputs();
    void finish();
    PipelineStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sse_stream
#endif
