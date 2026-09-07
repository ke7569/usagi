#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-/home/zane}"
RUN_MAIN="${RUN_MAIN:-${ROOT}/run_main}"
SYSTEM_JSON="${SZE_SYSTEM_JSON:-${ROOT}/configs/general_config/sze_system.json}"
PREPARE="${SZE_PREPARE_BIN:-${RUN_MAIN}/prepare_sze_runtime.py}"
MODE="${1:-strategy}"
DAY="${TRADING_DAY:-$(date +%Y%m%d)}"
DAILY_JSON="${2:-${SZE_DAILY_JSON:-${ROOT}/configs/config_sze_daily_${DAY}.json}}"

[[ -f "$SYSTEM_JSON" ]] || { echo "missing fixed system config: $SYSTEM_JSON" >&2; exit 1; }
[[ -f "$PREPARE" ]] || { echo "missing runtime planner: $PREPARE" >&2; exit 1; }
case "$MODE" in
  capture)
    python3 "$PREPARE" validate-system --system "$SYSTEM_JSON" --day "$DAY"
    ;;
  strategy)
    if [[ ! -f "$DAILY_JSON" && -e "${ROOT}/configs/current" ]]; then
      DAILY_JSON="${ROOT}/configs/current"
    fi
    [[ -f "$DAILY_JSON" ]] || { echo "missing exact-date daily config: $DAILY_JSON" >&2; exit 1; }
    python3 "$PREPARE" validate --system "$SYSTEM_JSON" --daily "$DAILY_JSON" --day "$DAY"
    ;;
  trade)
    if [[ ! -f "$DAILY_JSON" && -e "${ROOT}/configs/current" ]]; then
      DAILY_JSON="${ROOT}/configs/current"
    fi
    [[ -f "$DAILY_JSON" ]] || { echo "missing exact-date daily config: $DAILY_JSON" >&2; exit 1; }
    python3 "$PREPARE" validate-trade --system "$SYSTEM_JSON" --daily "$DAILY_JSON" --day "$DAY"
    ;;
  *)
    echo "usage: $0 capture|strategy|trade [DAILY_JSON]" >&2
    exit 2
    ;;
esac
