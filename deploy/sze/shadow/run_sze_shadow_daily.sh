#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-/home/zane}"
SOURCE_ROOT="${SZE_SHADOW_SOURCE_ROOT:-${ROOT}/usagi-sze-oms-20260907}"
DAY="${TRADING_DAY:-$(date +%Y%m%d)}"
JOURNAL="${SZE_JOURNAL_DIRECTORY:-${ROOT}/data/sze_journal_${DAY}}"
WORK="${SZE_SHADOW_WORK_ROOT:-/run/sze-shadow}/${DAY}"
CPU="${SZE_SHADOW_CPU:-40}"
BIN="$SOURCE_ROOT/build/sze-dev/t0_sze_stream"
PROFILE="$WORK/processing.paper.handoff.json"

[[ "$DAY" =~ ^[0-9]{8}$ ]] || { echo "invalid trading day: $DAY" >&2; exit 2; }
[[ "$CPU" =~ ^[0-9]+$ ]] || { echo "invalid shadow CPU: $CPU" >&2; exit 2; }
[[ -x "$BIN" ]] || { echo "missing stream binary: $BIN" >&2; exit 1; }
[[ -d "$JOURNAL" ]] || { echo "missing journal directory: $JOURNAL" >&2; exit 1; }
[[ -f "$PROFILE" ]] || { echo "missing stream profile: $PROFILE" >&2; exit 1; }

touch "$WORK/stream.stdout.log" "$WORK/stream.stderr.log"
echo "starting SZE paper shadow trading_day=${DAY} cpu=${CPU} execution=disabled" \
  >> "$WORK/stream.stderr.log"
exec /usr/bin/taskset -c "$CPU" "$BIN" \
  recovery-handoff "$JOURNAL" "$PROFILE" \
  >> "$WORK/stream.stdout.log" 2>> "$WORK/stream.stderr.log"
