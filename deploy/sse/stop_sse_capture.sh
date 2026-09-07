#!/usr/bin/env bash
set -euo pipefail

OUTPUT_DIR="${1:-${SSE_CAPTURE_OUTPUT_DIR:-./capture}}"
PID_FILE="$OUTPUT_DIR/sse_capture.pid"
if [[ ! -f "$PID_FILE" ]]; then
  echo "capture is not running"
  exit 0
fi
PID="$(cat "$PID_FILE")"
if kill -0 "$PID" 2>/dev/null; then
  kill -TERM "$PID"
  for _ in $(seq 1 30); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$PID" 2>/dev/null; then
    echo "capture did not stop cleanly pid=$PID" >&2
    exit 3
  fi
fi
rm -f "$PID_FILE"
echo "capture stopped"
