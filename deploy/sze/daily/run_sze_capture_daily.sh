#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-/home/zane}"
RUN_MAIN="${RUN_MAIN:-${ROOT}/run_main}"
SYSTEM_JSON="${SZE_SYSTEM_JSON:-${ROOT}/configs/general_config/sze_system.json}"
PREPARE="${SZE_PREPARE_BIN:-${RUN_MAIN}/prepare_sze_runtime.py}"
DAY="${TRADING_DAY:-$(date +%Y%m%d)}"
RUNTIME_ROOT="${SZE_RUNTIME_ROOT:-/run/sze}"
MAIN_BIN="${MAIN_BIN:-${ROOT}/bin/main}"
RUNTIME="${RUNTIME_ROOT}/${DAY}/capture"

[[ "$DAY" =~ ^[0-9]{8}$ ]] || { echo "invalid trading day: $DAY" >&2; exit 2; }
[[ -x "$MAIN_BIN" ]] || { echo "missing executable: $MAIN_BIN" >&2; exit 1; }
[[ -f "$SYSTEM_JSON" ]] || { echo "missing fixed system config: $SYSTEM_JSON" >&2; exit 1; }
[[ -f "$PREPARE" ]] || { echo "missing runtime planner: $PREPARE" >&2; exit 1; }
[[ -f "$RUN_MAIN/libsze_md.so" ]] || { echo "missing libsze_md.so" >&2; exit 1; }

python3 "$PREPARE" capture --system "$SYSTEM_JSON" --day "$DAY" \
  --runtime-root "$RUNTIME_ROOT"
[[ -f "$RUNTIME/deepwin.json" && -f "$RUNTIME/main.conf" ]] || {
  echo "capture runtime generation failed: $RUNTIME" >&2; exit 1;
}

mkdir -p /opt/deepwin/master/etc/deepwin /opt/deepwin/master/etc/log4cplus
install -m 0644 "$RUNTIME/deepwin.json" /opt/deepwin/master/etc/deepwin/deepwin.json
if [[ -f "$ROOT/configs/general_config/default.properties" ]]; then
  install -m 0644 "$ROOT/configs/general_config/default.properties" \
    /opt/deepwin/master/etc/log4cplus/default.properties
fi

export TZ="${TZ:-Asia/Shanghai}"
export LD_LIBRARY_PATH="$RUN_MAIN:${ROOT}/runtime_so/deepwin_core/lib/wingchun:${ROOT}/runtime_so/deepwin_core/lib/yijinjing:${ROOT}/runtime_so/third_party/wingchun:${ROOT}/runtime_so/third_party/boost:/opt/deepwin/master/lib/wingchun:/opt/deepwin/master/lib/yijinjing:/opt/deepwin/toolchain/boost-1.62.0/lib:${LD_LIBRARY_PATH:-}"
export EF_STACK_NAME="sze_capture_${DAY}"
export EF_RXQ_SIZE="${EF_RXQ_SIZE:-4096}"
export EF_RXQ_LIMIT="${EF_RXQ_LIMIT:-4096}"
export EF_MAX_PACKETS="${EF_MAX_PACKETS:-65536}"
export EF_MAX_RX_PACKETS="${EF_MAX_RX_PACKETS:-49152}"
export EF_PREALLOC_PACKETS="${EF_PREALLOC_PACKETS:-1}"
export EF_FREE_PACKETS_LOW_WATERMARK="${EF_FREE_PACKETS_LOW_WATERMARK:-2048}"
export EF_POLL_USEC="${EF_POLL_USEC:-20}"
export EF_INT_DRIVEN="${EF_INT_DRIVEN:-0}"

echo "starting fixed SZE capture trading_day=${DAY} daily_config_required=0"
if [[ "${SZE_RECEIVER_MODE:-onload}" == "onload" ]]; then
  exec /usr/bin/onload "$MAIN_BIN" "$RUNTIME/main.conf"
fi
exec "$MAIN_BIN" "$RUNTIME/main.conf"
