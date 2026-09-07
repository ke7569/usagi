#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build/deepwin-sze}"

export T0_DEEPWIN_RUNTIME_ROOT="${T0_DEEPWIN_RUNTIME_ROOT:-${RUNTIME_ROOT:-}}"
[[ -n "$T0_DEEPWIN_RUNTIME_ROOT" ]] || {
  echo "set T0_DEEPWIN_RUNTIME_ROOT (or RUNTIME_ROOT) to the validated Deepwin runtime root" >&2
  exit 2
}

"${ROOT}/verify_build_environment.sh"
cmake3 -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSZE_MARCH_NATIVE="${SZE_MARCH_NATIVE:-OFF}" \
  -DSZE_BUILD_TD="${SZE_BUILD_TD:-OFF}" \
  -DUSAGI_BUILD_DEEPWIN=ON \
  -DT0_BUILD_SZE_STREAM_PROCESSOR=ON \
  -DT0_BUILD_SSE_STREAM_PROCESSOR=OFF \
  -DRUNTIME="$T0_DEEPWIN_RUNTIME_ROOT" \
  -DSZE_TD_API_DIR="${SZE_TD_API_DIR:-${ROOT}/adapters/td/atp/api}"
TARGETS=(t0_strategy_sze sze_md sze_recovery_status sze_recovery_verify \
  sze_recoverable_test sze_protocol_test sze_config_guard_test)
if [[ "${SZE_BUILD_TD:-OFF}" == "ON" ]]; then
  TARGETS+=(sze_td)
fi
cmake3 --build "$BUILD_DIR" --target "${TARGETS[@]}" -- -j"${BUILD_JOBS:-4}"
TEST_REGEX='^(sze_recoverable_test|sze_protocol_test|sze_config_guard_test)$'
(cd "$BUILD_DIR" && ctest3 --output-on-failure -R "$TEST_REGEX")

nm -C "$BUILD_DIR/libt0_strategy_sze.so" | \
  grep -F 'typeinfo for kungfu::wingchun::IWCStrategy' >/dev/null
if nm -C "$BUILD_DIR/libt0_strategy_sze.so" | \
   grep -F 'typeinfo for IWCStrategy' >/dev/null; then
  echo "invalid global IWCStrategy ABI" >&2
  exit 1
fi
if ldd "$BUILD_DIR/libt0_strategy_sze.so" | grep -F 'not found'; then
  exit 1
fi
sha256sum "$BUILD_DIR/libt0_strategy_sze.so" "$BUILD_DIR/libsze_md.so"
if [[ "${SZE_BUILD_TD:-OFF}" == "ON" ]]; then
  sha256sum "$BUILD_DIR/libsze_td.so"
fi
