#!/bin/bash
# Compile an independent replay tool; do not touch production binaries/CMake.
set -euo pipefail
bench_build=${1:-build/sse-capture-release}
bench_binary=${2:-/tmp/sse-journal-throughput-benchmark}
g++ -std=gnu++11 -O3 -march=native \
  -I. -Isse -Icommon/stream -Isse/market_data -Isse/model \
  -Icommon/model/legacy -Ithird_party/eigen3 \
  tests/sse/journal_throughput_benchmark.cpp -o "$bench_binary" \
  -Wl,--start-group \
  "$bench_build/sse/libsse_stream_processing.a" \
  "$bench_build/sse/libsse_journal_core.a" \
  "$bench_build/sse/libsse_runtime_support.a" \
  "$bench_build/common/libdeepwin_market_data_runtime.a" \
  "$bench_build/sse/libsse_live_sampling.a" \
  "$bench_build/sse/libsse_hybrid_model_runtime.a" \
  "$bench_build/common/libsse_model_runtime.a" \
  "$bench_build/sse/libsse_snapshot_gru_runtime.a" \
  "$bench_build/common/libsnapshot_gru_engine.a" \
  "$bench_build/sse/libsse_v06_model.a" \
  "$bench_build/sse/libsse_auction59.a" \
  -Wl,--end-group -lpthread -lrt
