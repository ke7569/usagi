#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MD="$ROOT/sze/market_data"
OUT="$(dirname "${BASH_SOURCE[0]}")/sze_l1_snapshot_collector"

g++ -std=c++11 -O2 -Wall -Wextra -Wpedantic \
  -I"$ROOT" -I"$MD" -I"$ROOT/toolchain/deepwin_include" \
  "$(dirname "${BASH_SOURCE[0]}")/sze_l1_snapshot_collector.cpp" \
  "$MD/SZEProtocol.cpp" "$MD/SZERecoverable.cpp" \
  -pthread -lrt -o "$OUT"

echo "built $OUT"
