#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-/home/zane}"
SOURCE_ROOT="${SZE_SHADOW_SOURCE_ROOT:-${ROOT}/usagi-sze-oms-20260907}"
SYSTEM_JSON="${SZE_SYSTEM_JSON:-${ROOT}/configs/general_config/sze_system.json}"
STATUS_BIN="${SZE_RECOVERY_STATUS_BIN:-${ROOT}/bin/sze_recovery_status}"
DAY="${TRADING_DAY:-$(date +%Y%m%d)}"
DAILY_JSON="${SZE_DAILY_JSON:-${ROOT}/configs/config_sze_daily_${DAY}.json}"
JOURNAL="${SZE_JOURNAL_DIRECTORY:-${ROOT}/data/sze_journal_${DAY}}"
SHM="${SZE_SHM_PATH:-/dev/shm/sze_all_${DAY}.events}"
WORK="${SZE_SHADOW_WORK_ROOT:-/run/sze-shadow}/${DAY}"
ATTEMPTS="${SZE_SHADOW_PREPARE_ATTEMPTS:-20}"
INTERVAL="${SZE_SHADOW_PREPARE_INTERVAL_SECONDS:-5}"

[[ "$DAY" =~ ^[0-9]{8}$ ]] || { echo "invalid trading day: $DAY" >&2; exit 2; }
[[ "$ATTEMPTS" =~ ^[0-9]+$ && "$ATTEMPTS" -gt 0 ]] || {
  echo "invalid prepare attempts: $ATTEMPTS" >&2; exit 2;
}
[[ "$INTERVAL" =~ ^[0-9]+$ ]] || {
  echo "invalid prepare interval: $INTERVAL" >&2; exit 2;
}
[[ -f "$SYSTEM_JSON" ]] || { echo "missing system config: $SYSTEM_JSON" >&2; exit 1; }
[[ -f "$DAILY_JSON" ]] || { echo "missing daily config: $DAILY_JSON" >&2; exit 1; }
[[ -x "$STATUS_BIN" ]] || { echo "missing status binary: $STATUS_BIN" >&2; exit 1; }
[[ -f "$SOURCE_ROOT/deploy/sze/shadow/prepare_sze_shadow.py" ]] || {
  echo "missing shadow config generator" >&2; exit 1;
}
[[ -x "$SOURCE_ROOT/build/sze-dev/t0_sze_stream" ]] || {
  echo "missing shadow stream binary" >&2; exit 1;
}
EXPECTED_SOURCE_ID="$(python3 -c \
  'import json,sys; print(json.load(open(sys.argv[1]))["market_data"]["source_id"])' \
  "$SYSTEM_JSON")"
[[ "$EXPECTED_SOURCE_ID" =~ ^[0-9]+$ && "$EXPECTED_SOURCE_ID" -gt 0 ]] || {
  echo "invalid system market-data source ID" >&2; exit 1;
}

field() {
  local name="$1" token
  for token in $STATUS_OUTPUT; do
    case "$token" in
      "$name"=*) printf '%s\n' "${token#*=}"; return 0 ;;
    esac
  done
  return 1
}

STATUS_OUTPUT=""
GENERATION=""
for attempt in $(seq 1 "$ATTEMPTS"); do
  if [[ -d "$JOURNAL" && -f "$SHM" ]] && \
      STATUS_OUTPUT="$($STATUS_BIN "$SHM" 2>&1)"; then
    STATUS_DAY="$(field trading_day || true)"
    SOURCE_ID="$(field source_id || true)"
    PRODUCER_ALIVE="$(field producer_alive || true)"
    JOURNAL_DEGRADED="$(field journal_degraded || true)"
    INVALID_REASON="$(field invalid_reason_name || true)"
    GENERATION="$(field generation || true)"
    if [[ "$STATUS_DAY" == "$DAY" && "$SOURCE_ID" == "$EXPECTED_SOURCE_ID" && \
          "$PRODUCER_ALIVE" == "1" && "$JOURNAL_DEGRADED" == "0" && \
          "$INVALID_REASON" == "none" && "$GENERATION" =~ ^[0-9]+$ && \
          "$GENERATION" -gt 0 ]]; then
      break
    fi
  fi
  echo "shadow input not ready attempt=${attempt}/${ATTEMPTS}" >&2
  GENERATION=""
  sleep "$INTERVAL"
done
[[ -n "$GENERATION" ]] || {
  echo "capture input did not become healthy after ${ATTEMPTS} attempts" >&2
  [[ -n "$STATUS_OUTPUT" ]] && echo "$STATUS_OUTPUT" >&2
  exit 1
}

install -d -m 0750 "$WORK"
python3 -B "$SOURCE_ROOT/deploy/sze/shadow/prepare_sze_shadow.py" \
  --system "$SYSTEM_JSON" \
  --daily "$DAILY_JSON" \
  --day "$DAY" \
  --generation "$GENERATION" \
  --account-reference "sze-paper-shadow-${DAY}" \
  --unified-output "$WORK/unified.paper.live.json" \
  --profile-output "$WORK/processing.paper.handoff.json" \
  > "$WORK/prepare.json"

printf '%s\n' "$STATUS_OUTPUT" > "$WORK/capture.status"
echo "shadow profile prepared trading_day=${DAY} generation=${GENERATION}"
