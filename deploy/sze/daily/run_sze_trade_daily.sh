#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-/home/zane}"
RUN_MAIN="${RUN_MAIN:-${ROOT}/run_main}"
SYSTEM_JSON="${SZE_SYSTEM_JSON:-${ROOT}/configs/general_config/sze_system.json}"
DAY="${TRADING_DAY:-$(date +%Y%m%d)}"
RUNTIME_ROOT="${SZE_RUNTIME_ROOT:-/run/sze}"
RUNTIME="${RUNTIME_ROOT}/${DAY}"
MAIN_BIN="${MAIN_BIN:-${ROOT}/bin/main}"
TD_MERGER="${SZE_TD_MERGER:-${RUN_MAIN}/merge_sze_td_runtime.py}"

read -r CREDENTIALS TRADE_ENABLED <<EOF
$(python3 - "$SYSTEM_JSON" <<'PY'
import json, sys
d=json.load(open(sys.argv[1]))["trade"]
print(d.get("credentials_path", ""), int(bool(d.get("enabled", False))))
PY
)
EOF
[[ "$TRADE_ENABLED" == "1" ]] || { echo "trade disabled in fixed system config"; exit 0; }
[[ -f "$RUNTIME/strategy/trade/main.conf" ]] || { echo "no trade runtime for ${DAY}" >&2; exit 1; }
[[ -f "$CREDENTIALS" ]] || { echo "missing TD credentials" >&2; exit 1; }
[[ "$(stat -c '%a' "$CREDENTIALS")" == "600" ]] || { echo "TD credentials must have mode 600" >&2; exit 1; }
[[ -f "$TD_MERGER" ]] || { echo "missing TD runtime merger" >&2; exit 1; }

DEEPWIN_PRIVATE="$RUNTIME/strategy/trade/deepwin.json"
python3 "$TD_MERGER" "$RUNTIME/capture/deepwin.json" "$CREDENTIALS" \
  "$SYSTEM_JSON" "$DEEPWIN_PRIVATE"
install -m 0600 "$DEEPWIN_PRIVATE" /opt/deepwin/master/etc/deepwin/deepwin.json

export TZ="${TZ:-Asia/Shanghai}"
export LD_LIBRARY_PATH="$RUN_MAIN:${ROOT}/lib:${ROOT}/runtime_so/deepwin_core/lib/wingchun:${ROOT}/runtime_so/deepwin_core/lib/yijinjing:${ROOT}/runtime_so/third_party/wingchun:${ROOT}/runtime_so/third_party/boost:/opt/deepwin/master/lib/wingchun:/opt/deepwin/master/lib/yijinjing:/opt/deepwin/toolchain/boost-1.62.0/lib:${LD_LIBRARY_PATH:-}"
exec "$MAIN_BIN" "$RUNTIME/strategy/trade/main.conf"
