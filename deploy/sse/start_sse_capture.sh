#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 --site dongguan|jinqiao --interface-ip IP [--output-dir DIR]" >&2
  exit 2
}

SITE=""
INTERFACE_IP=""
OUTPUT_DIR="${SSE_CAPTURE_OUTPUT_DIR:-./capture}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --site) SITE="${2:-}"; shift 2 ;;
    --interface-ip) INTERFACE_IP="${2:-}"; shift 2 ;;
    --output-dir) OUTPUT_DIR="${2:-}"; shift 2 ;;
    *) usage ;;
  esac
done
[[ "$SITE" == "dongguan" || "$SITE" == "jinqiao" ]] || usage
[[ -n "$INTERFACE_IP" ]] || usage
[[ "$INTERFACE_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "invalid IPv4 interface" >&2; exit 2; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [[ -x "$SCRIPT_DIR/bin/sse_udp_observer" ]]; then
  BIN="${SSE_CAPTURE_BINARY:-$SCRIPT_DIR/bin/sse_udp_observer}"
elif [[ -f "$SCRIPT_DIR/../../CMakeLists.txt" ]]; then
  ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
  BIN="${SSE_CAPTURE_BINARY:-${USAGI_BUILD_DIR:-$ROOT/build/dev}/sse_udp_observer}"
else
  BIN="${SSE_CAPTURE_BINARY:-$SCRIPT_DIR/bin/sse_udp_observer}"
fi
[[ -x "$BIN" ]] || { echo "missing executable: $BIN" >&2; exit 3; }
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
PID_FILE="$OUTPUT_DIR/sse_capture.pid"
if [[ -f "$PID_FILE" ]]; then
  old_pid="$(cat "$PID_FILE")"
  if kill -0 "$old_pid" 2>/dev/null; then
    echo "capture already running pid=$old_pid" >&2
    exit 4
  fi
  rm -f "$PID_FILE"
fi

STAMP="$(date -u +%Y%m%d_%H%M%S)"
LOG="$OUTPUT_DIR/sse_udp_${SITE}_${STAMP}.jsonl"
ERR="$OUTPUT_DIR/sse_udp_${SITE}_${STAMP}.stderr.log"
ARGS=("$LOG")
if [[ "$SITE" == "jinqiao" ]]; then
  ARGS+=(snapshot_primary 239.35.80.5 37105
         snapshot_backup 239.57.80.5 37105
         tick_primary 239.35.80.9 37109
         tick_backup 239.57.80.9 37109)
else
  # The supplied server sheet lists SSE as primary/backup-consistent in Dongguan.
  ARGS+=(snapshot 239.57.80.5 37105
         tick 239.57.80.9 37109)
fi
ARGS+=(--interface-ip "$INTERFACE_IP")

nohup "$BIN" "${ARGS[@]}" >"$ERR" 2>&1 &
PID=$!
echo "$PID" >"$PID_FILE"
sleep 0.2
if ! kill -0 "$PID" 2>/dev/null; then
  echo "capture exited during startup; see $ERR" >&2
  rm -f "$PID_FILE"
  exit 5
fi
echo "pid=$PID"
echo "jsonl=$LOG"
echo "stderr=$ERR"
