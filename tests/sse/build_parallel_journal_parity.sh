#!/bin/bash
# Run from the repository root; leaves production binaries and CMake untouched.
set -euo pipefail
parity_build=${1:-build/sse-capture-release}
parity_binary=${2:-/tmp/sse-parallel-journal-parity}
g++ -std=gnu++11 -O3 -march=native -DSSE_PARITY_HAS_PARALLEL \
  -I. -Isse -Icommon/stream -Isse/market_data -Isse/model \
  -Icommon/model/legacy -Ithird_party/eigen3 \
  tests/sse/parallel_journal_parity.cpp -o "$parity_binary" \
  -Wl,--start-group \
  "$parity_build/sse/libsse_stream_processing.a" \
  "$parity_build/sse/libsse_journal_core.a" \
  "$parity_build/sse/libsse_runtime_support.a" \
  "$parity_build/common/libdeepwin_market_data_runtime.a" \
  "$parity_build/sse/libsse_live_sampling.a" \
  "$parity_build/sse/libsse_hybrid_model_runtime.a" \
  "$parity_build/common/libsse_model_runtime.a" \
  "$parity_build/sse/libsse_snapshot_gru_runtime.a" \
  "$parity_build/common/libsnapshot_gru_engine.a" \
  "$parity_build/sse/libsse_v06_model.a" \
  "$parity_build/sse/libsse_auction59.a" \
  -Wl,--end-group -lpthread -lrt
