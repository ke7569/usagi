#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 YYYYMMDD [output-dir]" >&2
  exit 2
fi

DATE="$1"
[[ "$DATE" =~ ^[0-9]{8}$ ]] || { echo "invalid trading date: $DATE" >&2; exit 2; }

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SOURCE_DIR="${SSE_DAILY_SOURCE_DIR:-/home/pta/live_config/guoxin-sh}"
OUT_DIR="${2:-${ROOT}/build/configs/sse-live-${DATE}}"
DAILY="${SOURCE_DIR}/config_sse_daily_${DATE}.json"
STOCK_DAY="${SOURCE_DIR}/stock_day_${DATE}.csv"
MANIFEST="${SOURCE_DIR}/stock_day_${DATE}.manifest.json"
TEMPLATE="${ROOT}/config/examples/sse/config_sse_hybrid_prediction_20260818.json"
MD_TEMPLATE="${ROOT}/config/examples/sse/sse_fpga_md_prediction_20260818.json"
TD_TEMPLATE="${ROOT}/config/examples/sse/sse_td_query.example.json"
STRATEGY="${SSE_STRATEGY:-}"
TD_LIB="${SSE_TD_LIB:-}"
MAIN_BIN="${SSE_MAIN_BIN:-}"
MODEL_ARCHIVE="${SSE_MODEL_ARCHIVE:-}"
DEPS_ARCHIVE="${SSE_DEPS_ARCHIVE:-}"
CHECK_SCRIPT="${ROOT}/deploy/sse/check_sse_live_runtime_package.sh"

for name in SSE_STRATEGY SSE_TD_LIB SSE_MAIN_BIN SSE_MODEL_ARCHIVE SSE_DEPS_ARCHIVE; do
  [[ -n "${!name:-}" ]] || {
    echo "set ${name} to a validated existing artifact; no legacy build/date default is provided" >&2
    exit 2
  }
done

for path in "$DAILY" "$STOCK_DAY" "$MANIFEST" "$TEMPLATE" "$MD_TEMPLATE" "$TD_TEMPLATE" "$STRATEGY" "$TD_LIB" "$MAIN_BIN" "$MODEL_ARCHIVE" "$DEPS_ARCHIVE" "$CHECK_SCRIPT"; do
  [[ -r "$path" ]] || { echo "missing required artifact: $path" >&2; exit 3; }
done
if [[ -e "$OUT_DIR" ]]; then
  echo "output already exists: $OUT_DIR" >&2
  exit 4
fi

mkdir -p "$OUT_DIR/bin" "$OUT_DIR/run_main/configs/general_config" \
  "$OUT_DIR/run_main/models" "$OUT_DIR/run_main/runtime" "$OUT_DIR/run_main/logs"

cp "$DAILY" "$OUT_DIR/run_main/configs/config_sse_daily_${DATE}.json"
cp "$STOCK_DAY" "$OUT_DIR/run_main/configs/stock_day_${DATE}.csv"
cp "$MANIFEST" "$OUT_DIR/run_main/configs/stock_day_${DATE}.manifest.json"
cp "$STRATEGY" "$OUT_DIR/run_main/libt0_strategy_sse.so"
cp "$TD_LIB" "$OUT_DIR/run_main/libsse_td.so"
cp "$MAIN_BIN" "$OUT_DIR/bin/main"
cp "$MAIN_BIN" "$OUT_DIR/bin/main.candidate"
cp "$MODEL_ARCHIVE" "$OUT_DIR/run_main/models/"
cp "$DEPS_ARCHIVE" "$OUT_DIR/run_main/runtime/"
cp "$CHECK_SCRIPT" "$OUT_DIR/check_package.sh"

python3 - "$DAILY" "$TEMPLATE" "$MD_TEMPLATE" "$TD_TEMPLATE" "$OUT_DIR/run_main/configs" "$DATE" <<'PY'
import json
import pathlib
import sys

daily_path, template_path, md_path, td_path, out_dir, date_text = sys.argv[1:]
date = int(date_text)
out = pathlib.Path(out_dir)
daily = json.loads(pathlib.Path(daily_path).read_text(encoding="utf-8"))
template = json.loads(pathlib.Path(template_path).read_text(encoding="utf-8"))
md = json.loads(pathlib.Path(md_path).read_text(encoding="utf-8"))
td = json.loads(pathlib.Path(td_path).read_text(encoding="utf-8"))

params = daily.get("ins_params")
if not isinstance(params, dict) or not params:
    raise SystemExit("daily config has no non-empty ins_params object")
codes = list(params)
for code, item in params.items():
    if not isinstance(item, dict):
        raise SystemExit("ins_params entry is not an object: %s" % code)
    required = ("Close", "HistoryAmount", "FreeShare", "HpUpperPrice",
                "HpLowerPrice", "HistoryVolatility20d", "Date", "static_position")
    missing = [key for key in required if key not in item]
    if missing:
        raise SystemExit("%s missing %s" % (code, ",".join(missing)))

cfg = template
cfg["strategy_name"] = "sse_hybrid_prediction_%s" % date_text
cfg["trading_day"] = date
cfg["static_data_source_date"] = daily.get("static_data_source_date")
cfg["static_data_hash"] = daily.get("static_data_hash")
cfg["ins_params"] = params
cfg["instrument_universe_mode"] = "all-decoded-sse-equities"
cfg["instrument_id"] = codes
cfg["his_amt"] = [params[c]["HistoryAmount"] for c in codes]
cfg["static_position"] = [params[c]["static_position"] for c in codes]
cfg["last_position"] = [0 for _ in codes]
cfg["prediction_capture"]["output"] = "/home/zane/log/sse/predictions-%s.jsonl" % date_text
cfg["deployment_required"] = [
    "install the model archive under /home/zane/run_main/models and verify its SHA256",
    "provide the target-host libsse_md.so and its Shanghai decoder/factor adapter",
    "set the actual market-data interface and multicast endpoints",
    "keep prediction_only=true, trading_enabled=false, production_approval=false",
    "td_source_index must remain empty for this prediction-only candidate",
]
(out / ("config_sse_prediction_%s.json" % date_text)).write_text(
    json.dumps(cfg, indent=2, ensure_ascii=True) + "\n")

md["trading_day"] = date
md["deployment_note"] = "Replace interface and endpoint values with the target Shanghai market-data sheet before startup."
(out / ("sse_fpga_md_%s.json" % date_text)).write_text(
    json.dumps(md, indent=2, ensure_ascii=True) + "\n")

td["trading_day"] = date
td["mode"] = "query-only"
td["deployment_note"] = "Credentials, account identifiers, client feature code, and both IX endpoints are placeholders; do not start until replaced."
(out / ("deepwin_sse_td_%s.example.json" % date_text)).write_text(
    json.dumps(td, indent=2, ensure_ascii=True) + "\n")

main = {
    "base_rid": 1400000,
    "vmd": [{
        "source": 89,
        "lib": "./libsse_md.so",
        "name": "sse_fpga",
        "config": "./configs/sse_fpga_md_%s.json" % date_text,
    }],
    "vtd": [],
    "vstr": [{
        "lib": "./libt0_strategy_sse.so",
        "config": "./configs/config_sse_prediction_%s.json" % date_text,
    }],
    "zmq": "tcp://127.0.0.1:6580",
}
(out / ("main_sse_prediction_%s.conf" % date_text)).write_text(
    json.dumps(main, indent=2, ensure_ascii=True) + "\n")

td_main = {
    "base_rid": 1900000,
    "vtd": [{"source": 190, "lib": "./libsse_td.so", "name": "sse_td"}],
    "zmq": "tcp://127.0.0.1:6580",
}
(out / ("main_sse_td_query_%s.conf" % date_text)).write_text(
    json.dumps(td_main, indent=2, ensure_ascii=True) + "\n")

with (out / ("static_positions_%s.csv" % date_text)).open("w") as handle:
    handle.write("instrument,static_position,Close,HistoryAmount,FreeShare\n")
    for code, item in params.items():
        if int(item["static_position"]) != 0:
            handle.write("%s,%s,%s,%s,%s\n" % (
                code, item["static_position"], item["Close"],
                item["HistoryAmount"], item["FreeShare"]))
PY

cp "$OUT_DIR/run_main/configs/deepwin_sse_td_${DATE}.example.json" \
  "$OUT_DIR/run_main/configs/general_config/deepwin_sse_td_${DATE}.example.json"

printf '%s\n' \
  "libsse_md.so is not present in this package." \
  "The target host must provide the vendor SSE decoder and frozen factor adapter." \
  "Do not replace this marker with an unrelated Shenzhen or observer library." \
  > "$OUT_DIR/MISSING_LIBSSE_MD.txt"
printf '%s\n' \
  "libt0_strategy_sse.so build is prediction scaffold and fail-closed." \
  "Current sse_get_obj returns nullptr until the native SSE event/factor adapter is wired." \
  "This binary must not be treated as an orderable Shanghai strategy." \
  > "$OUT_DIR/STRATEGY_BLOCKER.txt"

python3 - "$OUT_DIR" "$DATE" <<'PY'
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1]) / "run_main"
date = sys.argv[2]
daily = json.loads((root / "configs" / ("config_sse_daily_%s.json" % date)).read_text(encoding="utf-8"))
params = daily["ins_params"]
nonzero = {k: v for k, v in params.items() if int(v.get("static_position", 0)) != 0}
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
status = {
    "trading_day": int(date),
    "instrument_count": len(params),
    "static_position_nonzero_count": len(nonzero),
    "static_position_nonzero": sorted(nonzero),
    "daily_config_sha256": sha(root / "configs" / ("config_sse_daily_%s.json" % date)),
    "model_archive_sha256": sha(next((root / "models").glob("*.tar.gz"))),
    "strategy_build_id": "sse-strategy-v04-legacy-midmix-sse-20260812",
    "trading_enabled": False,
    "production_approval": False,
    "td_query_only": True,
    "libsse_md_present": False,
    "strategy_factory_ready": False,
    "safe_to_order": False,
}
(root.parent / "DEPLOYMENT_STATUS.json").write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")
PY

python3 - "$OUT_DIR" "$DATE" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
date = sys.argv[2]
count = len(json.loads((root / "run_main" / "configs" / ("config_sse_daily_%s.json" % date)).read_text(encoding="utf-8"))["ins_params"])
text = """Shanghai SSE runtime handoff for {date}
====================================

This directory contains the date-specific business/static data and safe
prediction/TD-query templates. It is not an orderable production package.

Validated locally:
- run_main/configs/config_sse_daily_{date}.json: {count} instruments
- run_main/configs/static_positions_{date}.csv: exact non-zero static_position audit
- trading_enabled=false, production_approval=false, vtd=[] in prediction config

Hard blockers:
- libsse_md.so is absent; the target host must provide the vendor Shanghai
  decoder and native factor adapter.
- The packaged SSE strategy library is fail-closed: sse_get_obj returns
  nullptr until that adapter is wired. It cannot generate live predictions.
- main.candidate is copied from the local Deepwin/BSE runtime and requires
  target-host ABI/RPATH/ldd verification before it is installed as main.

Prediction candidate:
  run_main/configs/main_sse_prediction_{date}.conf
  run_main/configs/config_sse_prediction_{date}.json
  run_main/configs/sse_fpga_md_{date}.json

Suggested copy layout on the Shanghai host (after host-side ldd/ABI check):
  bin/main -> /home/zane/bin/main
  run_main/* -> /home/zane/run_main/
  run_main/models/sse-hybrid-model-20260817.tar.gz -> extract under /home/zane/run_main/models/
  run_main/configs/general_config/deepwin_sse_td_{date}.example.json -> fill secrets and install as the host Deepwin TD config

Run `./check_package.sh <package-root>` after transfer. It intentionally exits
with code 10 while the two hard blockers remain.

TD query-only candidate (credentials and IX endpoints are placeholders):
  run_main/configs/main_sse_td_query_{date}.conf
  run_main/configs/deepwin_sse_td_{date}.example.json

The model archive is under run_main/models/. The Deepwin/ATP dependency handoff
is under run_main/runtime/. Use SHA256SUMS after copying. Do not enable TD or substitute a
Shenzhen MD plugin for libsse_md.so.
""".format(date=date, count=count)
(root / "README.md").write_text(text, encoding="utf-8")
PY

(cd "$OUT_DIR" && find . -type f ! -name SHA256SUMS ! -name MODEL_ARCHIVE.sha256 -print0 | sort -z | xargs -0 sha256sum) > "$OUT_DIR/SHA256SUMS"
sha256sum "$OUT_DIR"/run_main/models/*.tar.gz > "$OUT_DIR/run_main/models/MODEL_ARCHIVE.sha256"
chmod 0755 "$OUT_DIR/bin/main" "$OUT_DIR/bin/main.candidate" "$OUT_DIR/run_main/libt0_strategy_sse.so" "$OUT_DIR/run_main/libsse_td.so" "$OUT_DIR/check_package.sh"
echo "package=$OUT_DIR"
echo "sha256sums=$OUT_DIR/SHA256SUMS"
