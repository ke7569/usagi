#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MD="$ROOT/sze/market_data"
SELF="$(dirname "${BASH_SOURCE[0]}")"
OUT="$SELF/sze_snapshot_legacy15_predictor"

g++ -std=c++11 -O3 -mavx2 -mfma -Wall -Wextra -Wpedantic \
  -I"$ROOT" -I"$MD" -I"$ROOT/toolchain/deepwin_include" -I"$ROOT/third_party/eigen3" -I"$SELF" \
  "$SELF/sze_snapshot_legacy15_predictor.cpp" \
  "$SELF/snapshot_legacy15_model.cpp" "$SELF/snapshot_legacy15_factors.cpp" \
  "$MD/SZEProtocol.cpp" "$MD/SZERecoverable.cpp" \
  -pthread -lrt -o "$OUT"

echo "built $OUT"

g++ -std=c++11 -O3 -mavx2 -mfma -Wall -Wextra -Wpedantic \
  -I"$ROOT" -I"$ROOT/toolchain/deepwin_include" -I"$ROOT/third_party/eigen3" -I"$SELF" \
  "$SELF/benchmark_sze_snapshot_legacy15.cpp" "$SELF/snapshot_legacy15_model.cpp" \
  -o "$SELF/benchmark_sze_snapshot_legacy15"
echo "built $SELF/benchmark_sze_snapshot_legacy15"

g++ -std=c++11 -O3 -mavx2 -mfma -Wall -Wextra -Wpedantic \
  -I"$ROOT" -I"$ROOT/toolchain/deepwin_include" -I"$ROOT/third_party/eigen3" -I"$SELF" \
  "$SELF/validate_sze_snapshot_legacy15.cpp" "$SELF/snapshot_legacy15_model.cpp" \
  -o "$SELF/validate_sze_snapshot_legacy15"
echo "built $SELF/validate_sze_snapshot_legacy15"
