#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHON_BIN="${SZE_CONFIG_PYTHON:-/usr/bin/python3}"
GENERATOR="${ROOT_DIR}/tools/sze/generate_sze_live_config.py"
TEMPLATE_JSON="${SZE_TEMPLATE_JSON:-${ROOT_DIR}/config/examples/sze/config_sze_mix153060_live.template.json}"
DAILY_DIR="${SZE_DAILY_DIR:-/home/data/sze_daily}"
DAILY_PATTERN="${SZE_DAILY_PATTERN:-sze_daily_????????.csv}"
FREE_SHARE_FILE="${SZE_FREE_SHARE_FILE:-}"
OUTPUT_DIR="${SZE_OUTPUT_DIR:-${ROOT_DIR}/build/configs/sze_mix153060_live}"
MODE="${SZE_MODE:-hp-shadow}"
MD_SOURCE_INDEX="${SZE_MD_SOURCE_INDEX:-}"
TD_SOURCE_INDEX="${SZE_TD_SOURCE_INDEX:-}"
MAIN_CONF_TEMPLATE="${SZE_MAIN_CONF_TEMPLATE:-}"
STRATEGY_LIBRARY="${SZE_STRATEGY_LIBRARY:-./libt0_strategy_sze.so}"
RUNTIME_CONFIG_REF="${SZE_RUNTIME_CONFIG_REF:-}"
MODEL_PATH="${SZE_MODEL_PATH:-}"
EXPECTED_MODEL_SHA256="${SZE_EXPECTED_MODEL_SHA256:-}"
LOCK_FILE="${SZE_CONFIG_LOCK:-${OUTPUT_DIR}/.sze_live_config.lock}"

if [[ "${SZE_SCHEDULED_RUN:-0}" == "1" ]]; then
  SHANGHAI_WEEKDAY="$(TZ=Asia/Shanghai date +%u)"
  SHANGHAI_CLOCK="$(TZ=Asia/Shanghai date +%H:%M)"
  if [[ "${SHANGHAI_WEEKDAY}" -gt 5 || "${SHANGHAI_CLOCK}" != "00:30" ]]; then
    exit 0
  fi
fi

if [[ -z "${SZE_TARGET_DATE:-}" ]]; then
  TARGET_DATE="$(TZ=Asia/Shanghai date +%Y%m%d)"
else
  TARGET_DATE="${SZE_TARGET_DATE}"
fi

mkdir -p "$(dirname "${LOCK_FILE}")"
exec 9>"${LOCK_FILE}"
if ! flock -n 9; then
  echo "sze-live-config: another run is already active" >&2
  exit 4
fi

if [[ -n "${FREE_SHARE_FILE}" ]]; then
  FREE_SHARE_FILE="${FREE_SHARE_FILE//YYYYMMDD/${TARGET_DATE}}"
  FREE_SHARE_FILE="${FREE_SHARE_FILE//\{target_date\}/${TARGET_DATE}}"
fi
ARGS=(
  --target-date "${TARGET_DATE}"
  --template-json "${TEMPLATE_JSON}"
  --daily-dir "${DAILY_DIR}"
  --daily-pattern "${DAILY_PATTERN}"
  --output-dir "${OUTPUT_DIR}"
  --mode "${MODE}"
)
if [[ -n "${MD_SOURCE_INDEX}" ]]; then ARGS+=(--md-source-index "${MD_SOURCE_INDEX}"); fi
if [[ -n "${TD_SOURCE_INDEX}" ]]; then ARGS+=(--td-source-index "${TD_SOURCE_INDEX}"); fi
if [[ -n "${MAIN_CONF_TEMPLATE}" ]]; then ARGS+=(--main-conf-template "${MAIN_CONF_TEMPLATE}"); fi
if [[ -n "${STRATEGY_LIBRARY}" ]]; then ARGS+=(--strategy-library "${STRATEGY_LIBRARY}"); fi
if [[ -n "${RUNTIME_CONFIG_REF}" ]]; then ARGS+=(--runtime-config-ref "${RUNTIME_CONFIG_REF}"); fi
if [[ -n "${MODEL_PATH}" ]]; then ARGS+=(--model-path "${MODEL_PATH}"); fi
if [[ -n "${EXPECTED_MODEL_SHA256}" ]]; then ARGS+=(--expected-model-sha256 "${EXPECTED_MODEL_SHA256}"); fi
if [[ -n "${FREE_SHARE_FILE}" ]]; then ARGS+=(--free-share-file "${FREE_SHARE_FILE}"); fi

exec "${PYTHON_BIN}" "${GENERATOR}" "${ARGS[@]}"
