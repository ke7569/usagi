#ifndef SSE_T0_STREAM_PROCESSOR_H
#define SSE_T0_STREAM_PROCESSOR_H

#include "sse/sampling/sse_batch_end_sampler.h"
#include "sse/market_data/sse_primary_decoder.h"
#include "sse/factors/sse_tick_factors.h"
#include "sse/market_data/sse_tick_static_metadata.h"
#include "sse/market_data/sse_tick_order_book.h"
#include "sse/factors/snapshot36.h"
#include "sse/model/sse_hybrid_model.h"
#include "common/stream/MarketDataStream.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sse_stream {

static const std::uint64_t kSnapshotGenerateStartMicros = 34200000000ULL;
static const std::uint64_t kSnapshotGenerateEndMicros = 34860000000ULL;

struct Provenance {
    deepwin_market_data::StreamEventKind stream_kind;
    std::uint64_t stream_sequence;
    std::uint64_t monotonic_ns;
    std::uint64_t realtime_ns;
    std::uint64_t receive_batch;
    std::uint32_t batch_index;
    std::uint32_t batch_size;
    std::uint32_t stream_channel_id;
    std::uint32_t wire_channel_no;
    std::uint64_t wire_sequence;
    std::size_t record_offset;
    std::uint32_t source_ipv4;
    std::uint16_t source_port;
    std::uint16_t timestamp_flags;
    std::uint64_t batch_id;
    std::uint64_t batch_emitted_ns;
    sse_live_sampling::BatchCloseReason batch_close_reason;

    Provenance();
};

struct TickOutput {
    sse_live::TickEvent event;
    sse_tick::FactorRow factors;
    std::vector<sse_tick::Level> bid_levels;
    std::vector<sse_tick::Level> ask_levels;
    double last_trade_price;
    double total_trade_volume;
    double total_trade_turnover;
    sse_hybrid_model::Prediction prediction;
    bool prediction_valid;
    sse_live_sampling::SampleDecision sample_decision;
    Provenance provenance;

    TickOutput();
};

struct SnapshotOutput {
    sse_live::Snapshot snapshot;
    std::vector<float> snapshot36;
    std::vector<float> auction59;
    sse_hybrid_model::Prediction prediction;
    bool prediction_valid;
    Provenance provenance;

    SnapshotOutput();
};

struct BatchEndOutput {
    std::uint64_t batch_id;
    std::uint64_t last_hardware_ns;
    std::uint64_t emitted_monotonic_ns;
    std::uint32_t packet_count;
    std::uint32_t candidate_count;
    std::uint32_t prediction_count;

    BatchEndOutput();
};

enum OutputKind { kTickOutput = 1, kSnapshotOutput = 2, kBatchEndOutput = 3 };

struct Output {
    OutputKind kind;
    TickOutput tick;
    SnapshotOutput snapshot;
    BatchEndOutput batch_end;

    Output();
};

typedef std::function<void(const Output&)> OutputCallback;
typedef std::function<bool(const std::string&, std::uint64_t,
                           std::vector<float>*, std::string*)> Auction59Provider;

class SseStreamProcessor {
public:
    // metadata is copied and normalized once, so live and replay share the
    // same bounded per-security state. A supplied model must already be loaded.
    SseStreamProcessor(const sse_tick::DailyStaticMetadataMap& metadata,
                       const sse_hybrid_model::Model* model,
                       bool factors_only,
                       const OutputCallback& callback,
                       const Auction59Provider& auction59_provider = Auction59Provider());

    // Processes one live or replay event. Invalid input is sticky: this method
    // throws std::runtime_error and every subsequent call throws the same error.
    void on_event(const deepwin_market_data::StreamEvent& event);

    bool invalid() const { return invalid_; }
    const std::string& invalid_reason() const { return invalid_reason_; }

private:
    struct InstrumentState {
        InstrumentState(const std::string& code,
                        const sse_tick::DailyStaticMetadata& metadata);
        sse_tick::OrderBook book;
        sse_tick::FactorState factors;
        sse_live_sampling::TickSampleGate sample_gate;
        sse_hybrid_model::State model_state;
        sse_live::Snapshot previous_snapshot;
        bool have_previous_snapshot;
        std::uint64_t last_snapshot_time;
        sse_live::TickEvent pending_tick;
        Provenance pending_provenance;
        bool have_pending_tick;
    };

    struct ChannelSequence {
        std::uint64_t last_tick;
        bool have_tick;
        ChannelSequence() : last_tick(0ULL), have_tick(false) {}
    };

    typedef std::map<std::string, InstrumentState> StateMap;
    typedef std::map<std::uint32_t, ChannelSequence> SequenceMap;

    void fail(const std::string& reason);
    void on_datagram(const deepwin_market_data::StreamEvent& event);
    void on_idle(const deepwin_market_data::StreamEvent& event);
    void process_tick(const sse_live::TickEvent& tick,
                      const deepwin_market_data::StreamEvent& event,
                      std::size_t record_offset);
    void process_snapshot(const sse_live::Snapshot& snapshot,
                          const deepwin_market_data::StreamEvent& event,
                          std::size_t record_offset);
    void process_closed_batch(const sse_live_sampling::BatchEnd& batch);
    void advance_hardware_batch(const deepwin_market_data::StreamEvent& event);
    void close_hardware_batch(std::uint64_t emitted_monotonic_ns,
                              sse_live_sampling::BatchCloseReason reason);
    void commit_hardware_candidate(const sse_live_sampling::Candidate& candidate);
    sse_live_sampling::TickCut book_cut(const InstrumentState& state,
                                      const sse_live::TickEvent& tick) const;
    void initialize_window(InstrumentState& state, const sse_live::TickEvent& tick);
    Provenance provenance(const deepwin_market_data::StreamEvent& event,
                          std::uint32_t wire_channel,
                          std::uint64_t wire_sequence,
                          std::size_t record_offset) const;
    InstrumentState* state_for(const std::string& code);
    bool valid_tick_sequence(const sse_live::TickEvent& tick);
    static bool valid_auction59(const std::vector<float>& factors);

    StateMap states_;
    SequenceMap channel_sequences_;
    sse_live_sampling::BatchEndSampler batch_sampler_;
    std::vector<sse_live_sampling::BatchEnd> closed_batches_;
    bool hardware_batch_mode_;
    bool hardware_batch_open_;
    std::uint64_t hardware_batch_id_;
    std::uint64_t next_hardware_batch_id_;
    std::uint64_t last_hardware_ns_;
    std::uint64_t last_hardware_monotonic_ns_;
    std::uint32_t hardware_batch_packet_count_;
    std::map<std::string, sse_live_sampling::Candidate> hardware_candidates_;
    const sse_hybrid_model::Model* model_;
    bool factors_only_;
    OutputCallback callback_;
    Auction59Provider auction59_provider_;
    std::map<std::string, std::vector<float> > auction59_inputs_;
    bool invalid_;
    std::string invalid_reason_;
};

}  // namespace sse_stream

#endif  // SSE_T0_STREAM_PROCESSOR_H
