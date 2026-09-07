#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-/home/zane}"
RUN_MAIN="${RUN_MAIN:-${ROOT}/run_main}"
SYSTEM_JSON="${SZE_SYSTEM_JSON:-${ROOT}/configs/general_config/sze_system.json}"
PREPARE="${SZE_PREPARE_BIN:-${RUN_MAIN}/prepare_sze_runtime.py}"
DAY="${TRADING_DAY:-$(date +%Y%m%d)}"
RUNTIME_ROOT="${SZE_RUNTIME_ROOT:-/run/sze}"
DAILY_JSON="${1:-${SZE_DAILY_JSON:-${ROOT}/configs/config_sze_daily_${DAY}.json}}"
MAIN_BIN="${MAIN_BIN:-${ROOT}/bin/main}"
RUNTIME="${RUNTIME_ROOT}/${DAY}/strategy"
READY_FILE="$RUNTIME/recovery.ready"

if [[ ! -f "$DAILY_JSON" && -e "${ROOT}/configs/current" ]]; then
  DAILY_JSON="${ROOT}/configs/current"
fi
[[ -f "$DAILY_JSON" ]] || { echo "missing exact-date daily config: $DAILY_JSON" >&2; exit 1; }
[[ -f "$SYSTEM_JSON" ]] || { echo "missing fixed system config: $SYSTEM_JSON" >&2; exit 1; }
[[ -x "$MAIN_BIN" ]] || { echo "missing executable: $MAIN_BIN" >&2; exit 1; }

PREPARE_ARGS=(strategy --system "$SYSTEM_JSON" --daily "$DAILY_JSON"
  --day "$DAY" --runtime-root "$RUNTIME_ROOT")
python3 "$PREPARE" "${PREPARE_ARGS[@]}"

export TZ="${TZ:-Asia/Shanghai}"
export LD_LIBRARY_PATH="$RUN_MAIN:${ROOT}/runtime_so/deepwin_core/lib/wingchun:${ROOT}/runtime_so/deepwin_core/lib/yijinjing:${ROOT}/runtime_so/third_party/wingchun:${ROOT}/runtime_so/third_party/boost:/opt/deepwin/master/lib/wingchun:/opt/deepwin/master/lib/yijinjing:/opt/deepwin/toolchain/boost-1.62.0/lib:${LD_LIBRARY_PATH:-}"
PIDS=()
cleanup() {
  trap - TERM INT EXIT
  rm -f "$READY_FILE"
  for pid in "${PIDS[@]:-}"; do kill -TERM "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]:-}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup TERM INT EXIT

for conf in "$RUNTIME"/workers/main_*.conf; do
  [[ -f "$conf" ]] || continue
  "$MAIN_BIN" "$conf" >"${conf%.conf}.log" 2>&1 &
  PIDS+=("$!")
done
EXPECTED="$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["shards"]))' "$RUNTIME/manifest.json")"
[[ "${#PIDS[@]}" -eq "$EXPECTED" ]] || {
  echo "expected $EXPECTED workers, started ${#PIDS[@]}" >&2; exit 1;
}
sleep 1
for pid in "${PIDS[@]}"; do
  kill -0 "$pid" 2>/dev/null || { echo "SZE recovery worker failed during startup pid=$pid" >&2; exit 1; }
done
printf 'trading_day=%s workers=%s\n' "$DAY" "${#PIDS[@]}" >"${READY_FILE}.new"
mv -f "${READY_FILE}.new" "$READY_FILE"
echo "SZE recovery started trading_day=${DAY} workers=${#PIDS[@]} runtime=${RUNTIME}"
while :; do
  for pid in "${PIDS[@]}"; do
    kill -0 "$pid" 2>/dev/null || { echo "SZE recovery worker exited pid=$pid" >&2; exit 1; }
  done
  sleep 1
done
