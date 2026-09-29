#!/usr/bin/env bash
# Build once, test those exact files, then install them. Never restart capture.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd -P)
build_dir="$repo/build/sse-capture-release"
branch=sse-dev
check_only=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --build) build_dir=$2; shift 2;;
    --branch) branch=$2; shift 2;;
    --check-only) check_only=1; shift;;
    -h|--help) echo "usage: $0 [--build DIR] [--branch sse-dev] [--check-only]"; exit 0;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done
actual_branch=$(cd "$repo" && git symbolic-ref --short HEAD)
[ "$actual_branch" = "$branch" ] || { echo "expected branch $branch, found $actual_branch" >&2; exit 1; }
cmake_bin="${CMAKE:-/opt/cmake3/usr/bin/cmake3}"
ctest_bin="${CTEST:-/opt/cmake3/usr/bin/ctest3}"
jobs="${JOBS:-2}"
bin_dir="${SSE_BIN_DIR:-/home/zane/usagi-bin}"
service_dir="${SSE_SERVICE_DIR:-/etc/systemd/system}"
mkdir -p "$build_dir"
build_dir=$(cd "$build_dir" && pwd -P)
(cd "$build_dir" && "$cmake_bin" "$repo" -DSZE_MARCH_NATIVE=ON -DT0_BUILD_SZE_STREAM_PROCESSOR=OFF -DSSE_BUILD_TD=OFF -DSZE_BUILD_TD=OFF -DUSAGI_BUILD_DEEPWIN=OFF)
"$cmake_bin" --build "$build_dir" --target t0_sse_journal_capture t0_sse_journal_predict sse_hwstamp_ctl sse_journal_core_test sse_journal_handoff_test -- -j"$jobs"
(cd "$build_dir" && "$ctest_bin" --output-on-failure -R '^sse_journal_(core|handoff|integration)_test$')
python2 "$repo/tests/sse/test_capture_retention.py"
python2 "$repo/tests/sse/test_capture_check.py"
python2 "$repo/tests/sse/sse_journal_launcher_test.py" \
  --launcher "$repo/tools/sse/run_journal_capture.py" --binary "$build_dir/t0_sse_journal_capture"
python2 - "$repo/deploy/sse/journal_capture.live.json" <<'PY'
import json, sys
with open(sys.argv[1]) as source:
    value = json.load(source)
rows = value.get('channels', [])
if len(rows) != 2 or set(row.get('name') for row in rows) != set(('sse_tick', 'sse_snapshot')):
    raise SystemExit('capture requires sse_tick and sse_snapshot channels')
for row in rows:
    if not row.get('group') or not row.get('interface_ip') or not 1 <= row.get('port', 0) <= 65535:
        raise SystemExit('invalid capture channel')
PY
[ "$check_only" = 0 ] || { echo 'Capture build and tests passed; nothing installed.'; exit 0; }
mkdir -p "$bin_dir" "$service_dir"
stage=$(mktemp -d "$bin_dir/.capture-release.XXXXXX")
service_stage=''
cleanup() {
  # Only this invocation's fresh staging directory and temporary unit file.
  rm -f "$stage/t0_sse_journal_capture" "$stage/sse_hwstamp_ctl"
  [ ! -d "$stage" ] || rmdir "$stage"
  [ -z "$service_stage" ] || rm -f "$service_stage"
}
trap cleanup EXIT
install -m 0755 "$build_dir/t0_sse_journal_capture" "$stage/t0_sse_journal_capture"
install -m 0755 "$build_dir/sse_hwstamp_ctl" "$stage/sse_hwstamp_ctl"
# An existing process keeps its original executable mapping across each rename.
mv -f "$stage/t0_sse_journal_capture" "$bin_dir/t0_sse_journal_capture"
mv -f "$stage/sse_hwstamp_ctl" "$bin_dir/sse_hwstamp_ctl"
service_stage=$(mktemp "$service_dir/.sse-journal-capture.service.XXXXXX")
install -m 0644 "$repo/deploy/sse/sse-journal-capture.service" "$service_stage"
mv -f "$service_stage" "$service_dir/sse-journal-capture.service"
service_stage=''
systemctl daemon-reload
echo "Installed tested capture files. Config remains $repo/deploy/sse/journal_capture.live.json."
echo 'Capture was NOT restarted. The next scheduled start uses this version.'
