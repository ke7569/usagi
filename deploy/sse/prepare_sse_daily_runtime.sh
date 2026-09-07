#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: prepare_sse_daily_runtime.sh YYYYMMDD SESSION_ROOT" >&2
  exit 2
fi

DATE=$1
SESSION_ROOT=$2
SOURCE_ROOT="$SESSION_ROOT/source"
GENERATED_ROOT="$SESSION_ROOT/generated"
CONFIG="$SOURCE_ROOT/config_sse_daily_${DATE}.json"
STOCK_DAY="$SOURCE_ROOT/stock_day_${DATE}.csv"
MANIFEST="$SOURCE_ROOT/stock_day_${DATE}.manifest.json"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILDER="${SSE_DAILY_CONFIG_BUILDER:-${USAGI_BUILD_DIR:-$ROOT/build/dev}/sse_daily_config_builder}"
STATIC_PREP="$(cd "$(dirname "$0")" && pwd)/prepare_sse_static_metadata.sh"
VALIDATOR="${SSE_STATIC_METADATA_VALIDATOR:-}"

[[ "$DATE" =~ ^[0-9]{8}$ ]] || { echo "invalid date: $DATE" >&2; exit 3; }
[[ -n "$VALIDATOR" && -x "$VALIDATOR" ]] || {
  echo "SSE_STATIC_METADATA_VALIDATOR must name the validated external metadata checker" >&2
  exit 5
}
for path in "$CONFIG" "$STOCK_DAY" "$MANIFEST" "$BUILDER" "$STATIC_PREP"; do
  [[ -r "$path" ]] || { echo "missing required file: $path" >&2; exit 3; }
done

mkdir -p "$GENERATED_ROOT"

hash_from_manifest() {
  local artifact=$1
  sed -n "/\"${artifact}\"/,/}/p" "$MANIFEST" |
    sed -n 's/.*"sha256"[[:space:]]*:[[:space:]]*"\([0-9a-fA-F]*\)".*/\1/p' | head -1
}

verify_artifact() {
  local path=$1
  local artifact=$2
  local expected actual
  expected=$(hash_from_manifest "$artifact")
  actual=$(sha256sum "$path" | awk '{print $1}')
  [[ -n "$expected" && "$actual" == "$expected" ]] || {
    echo "hash mismatch for $artifact expected=$expected actual=$actual" >&2
    exit 4
  }
  echo "$artifact sha256=$actual"
}

verify_artifact "$CONFIG" "config_sse_daily_${DATE}.json"
verify_artifact "$STOCK_DAY" "stock_day_${DATE}.csv"

"$BUILDER" "$CONFIG" "$GENERATED_ROOT/config_sse_staged_${DATE}.json" "$DATE"
"$STATIC_PREP" "$STOCK_DAY" "$DATE" "$GENERATED_ROOT/sse_static_${DATE}.csv"

"$VALIDATOR" \
  "$GENERATED_ROOT/sse_static_${DATE}.csv" "$DATE"
cat > "$GENERATED_ROOT/sse_static_${DATE}.report.json" <<EOF
{
  "trading_day": $DATE,
  "source": "stock_day_${DATE}.csv",
  "security_count": $(($(wc -l < "$GENERATED_ROOT/sse_static_${DATE}.csv") - 1)),
  "static_metadata_sha256": "$(sha256sum "$GENERATED_ROOT/sse_static_${DATE}.csv" | awk '{print $1}')",
  "limits_complete": true,
  "ipo_first_day_count": 0
}
EOF

cat > "$GENERATED_ROOT/PREPARED_MANIFEST_${DATE}.txt" <<EOF
trading_day=$DATE
source_root=$SOURCE_ROOT
staged_config=$GENERATED_ROOT/config_sse_staged_${DATE}.json
static_metadata=$GENERATED_ROOT/sse_static_${DATE}.csv
static_report=$GENERATED_ROOT/sse_static_${DATE}.report.json
config_sha256=$(sha256sum "$CONFIG" | awk '{print $1}')
stock_day_sha256=$(sha256sum "$STOCK_DAY" | awk '{print $1}')
staged_config_sha256=$(sha256sum "$GENERATED_ROOT/config_sse_staged_${DATE}.json" | awk '{print $1}')
static_metadata_sha256=$(sha256sum "$GENERATED_ROOT/sse_static_${DATE}.csv" | awk '{print $1}')
static_report_sha256=$(sha256sum "$GENERATED_ROOT/sse_static_${DATE}.report.json" | awk '{print $1}')
instrument_count=$(grep -c '"Date"[[:space:]]*:[[:space:]]*'"$DATE" "$GENERATED_ROOT/config_sse_staged_${DATE}.json")
trading_enabled=false
production_approval=false
startup_cancel_all_orders=false
EOF
chmod 0640 "$GENERATED_ROOT"/*
echo "SSE daily staged runtime prepared under $GENERATED_ROOT"
