#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
API_DIR="${SSE_TD_API_DIR:-$ROOT/modules/deepwin_guoxin/api}"
DEEPWIN_ROOT="${DEEPWIN_ROOT:-/opt/deepwin}"

missing=0
require_file() {
  if [[ ! -r "$1" ]]; then
    echo "MISSING $1" >&2
    missing=1
  else
    echo "FOUND   $1"
  fi
}

echo "SSE TD environment check"
echo "root=$ROOT"
echo "api_dir=$API_DIR"
echo "deepwin_root=$DEEPWIN_ROOT"
require_file "$API_DIR/include/atp_quant_api.h"
require_file "$API_DIR/lib/libatpquantapi.so"
require_file "$DEEPWIN_ROOT/master/include/ITDEngine.h"
require_file "$DEEPWIN_ROOT/master/include/longfist/LFConstants.h"
require_file "$DEEPWIN_ROOT/master/lib/wingchun/libwingchuntd.so"
require_file "$DEEPWIN_ROOT/master/lib/yijinjing/libjournal.so.1.1"
require_file "$DEEPWIN_ROOT/master/lib/yijinjing/libkflog.so"

if [[ "$missing" -ne 0 ]]; then
  echo "SSE TD environment is incomplete; no build or order may be attempted" >&2
  exit 2
fi
echo "SSE TD environment: READY_FOR_BUILD"
