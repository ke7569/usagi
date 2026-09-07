#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: prepare_sse_static_metadata.sh STOCK_DAY_CSV YYYYMMDD OUTPUT_CSV" >&2
  exit 2
fi

INPUT=$1
DATE=$2
OUTPUT=$3
[[ -r "$INPUT" ]] || { echo "missing stock_day CSV: $INPUT" >&2; exit 3; }
[[ "$DATE" =~ ^[0-9]{8}$ ]] || { echo "invalid date: $DATE" >&2; exit 3; }
mkdir -p "$(dirname "$OUTPUT")"

tmp="${OUTPUT}.tmp.$$"
trap 'rm -f "$tmp"' EXIT
awk -F',' -v OFS=',' -v expected_date="$DATE" '
  BEGIN {
    print "security_id,name,date,pre_close,upper_limit,lower_limit,listing_date,is_ipo_first_day,limits_valid,source,quality,error"
  }
  NR == 1 { next }
  NF < 9 { next }
  {
    code=$2
    sub(/\r$/, "", $9)
    valid=(length(code)==9 && (substr(code,1,2)=="60" || substr(code,1,2)=="68") &&
           substr(code,7,1)=="." && substr(code,8,2)=="SH")
    if (!valid || $1 != expected_date) next
    ok=($7+0 > 0 && $8+0 > 0 && $9+0 > 0)
    print substr(code,1,6), "", $1, $7, $8, $9, 0, 0, (ok ? 1 : 0),
          "stock_day_" expected_date ".csv", (ok ? "daily" : "invalid_limits"), ""
    count++
  }
  END {
    if (count == 0) exit 10
    print "rows=" count > "/dev/stderr"
  }
' "$INPUT" > "$tmp"
mv -f "$tmp" "$OUTPUT"
trap - EXIT
chmod 0644 "$OUTPUT"
