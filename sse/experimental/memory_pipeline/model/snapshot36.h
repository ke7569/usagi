#ifndef SSE_T0_SNAPSHOT36_H
#define SSE_T0_SNAPSHOT36_H

#include <vector>

#include "../market_data/sse_primary_decoder.h"

namespace sse_snapshot36 {

bool valid(const sse_live::Snapshot& snapshot);
std::vector<float> build(const sse_live::Snapshot& start,
                         const sse_live::Snapshot& current);

}  // namespace sse_snapshot36

#endif  // SSE_T0_SNAPSHOT36_H
