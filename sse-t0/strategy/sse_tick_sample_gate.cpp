#include "sse_tick_sample_gate.h"

#include "../market_data/sse_tick_units.h"

namespace sse_tick_strategy {

SampleGateInput::SampleGateInput()
    : batch_trade_shares(0U), batch_turnover_yuan(0.0),
      amount_threshold_yuan(0.0), quiet_batch_end(false), mid_changed(false) {}

SampleGateResult evaluate_sample_gate(const SampleGateInput& input,
                                      const char** reason) {
    if (reason) *reason = "sample_condition_not_met";
    if (!input.quiet_batch_end) {
        if (reason) *reason = "not_batch_end";
        return kSampleGateNoSample;
    }
    // Hard gate: real traded shares in this batch.
    if (input.batch_trade_shares < sse_tick::kMinBatchTradeShares) {
        if (reason) *reason = "trade_volume_below_100";
        return kSampleGateTradeVolumeBelow100;
    }
    const bool amount_ok = input.amount_threshold_yuan > 0.0 &&
                           input.batch_turnover_yuan > input.amount_threshold_yuan;
    if (amount_ok || input.mid_changed) {
        if (reason) *reason = "sample";
        return kSampleGateSample;
    }
    if (reason) *reason = "sample_condition_not_met";
    return kSampleGateNoSample;
}

}  // namespace sse_tick_strategy
