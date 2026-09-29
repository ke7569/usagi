#ifndef SSE_T0_TICK_SAMPLE_GATE_H
#define SSE_T0_TICK_SAMPLE_GATE_H

// Sampling gate for the CompleteOrderBookSH tick model.  Pure logic with no
// engine dependency so the gate rules can be unit-tested directly.
//
// Contract (canonical units, see sse_tick_units.h):
//   1. A batch must contain real trades totalling >= 100 shares
//      (batch_trade_shares).  Price changes or order/deletion activity alone
//      can never produce a sample.
//   2. Given the 100-share hard gate, the batch samples when either its
//      trade turnover exceeds the daily amount threshold OR the mid price
//      changed over the batch.  A price change may not replace the 100-share
//      trade requirement, and the amount rule may not bypass it either.
//   3. The quiet-batch flag is set by the caller only after the security has
//      been quiet (per-stock NIC arrival time) for 100us.
#include <cstdint>

namespace sse_tick_strategy {

struct SampleGateInput {
    std::uint64_t batch_trade_shares;
    double batch_turnover_yuan;
    double amount_threshold_yuan;  // <=0 disables the amount arm
    bool quiet_batch_end;
    bool mid_changed;
    SampleGateInput();
};

enum SampleGateResult {
    kSampleGateSample,
    kSampleGateTradeVolumeBelow100,
    kSampleGateNoSample
};

SampleGateResult evaluate_sample_gate(const SampleGateInput& input,
                                      const char** reason);

}  // namespace sse_tick_strategy

#endif  // SSE_T0_TICK_SAMPLE_GATE_H
