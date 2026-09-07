#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "$0")" && pwd)}"
STATUS="$ROOT/DEPLOYMENT_STATUS.json"

[[ -d "$ROOT" && -f "$ROOT/SHA256SUMS" ]] || {
  echo "invalid SSE package root: $ROOT" >&2
  exit 2
}
(cd "$ROOT" && sha256sum -c SHA256SUMS)

python3 - "$ROOT" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
status = json.loads((root / "DEPLOYMENT_STATUS.json").read_text(encoding="utf-8"))
config_name = "config_sse_prediction_%s.json" % status["trading_day"]
config = json.loads((root / "run_main/configs" / config_name).read_text(encoding="utf-8"))
params = config.get("ins_params", {})
assert len(params) == status["instrument_count"]
assert sum(int(v.get("static_position", 0)) != 0 for v in params.values()) == status["static_position_nonzero_count"]
assert config["trading_enabled"] is False
assert config["production_approval"] is False
assert config["td_source_index"] == []
print("static_config=PASS instruments=%d nonzero_positions=%d" % (len(params), status["static_position_nonzero_count"]))
PY

echo "package_root=$ROOT"
echo "libsse_md.so=missing (target host must supply the Shanghai decoder/factor adapter)"
echo "strategy_factory=blocked (current library is fail-closed; sse_get_obj returns nullptr)"
echo "safe_to_order=false"
exit 10
