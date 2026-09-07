#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 --handoff DIR_OR_TAR_ZST --output-dir DIR [--package-id ID]" >&2
  exit 2
}

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
handoff=""
output_dir=""
package_id="sse-snapshot-gru-20260817"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --handoff) handoff=${2:-}; shift 2 ;;
    --output-dir) output_dir=${2:-}; shift 2 ;;
    --package-id) package_id=${2:-}; shift 2 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done
[[ -n "$handoff" && -n "$output_dir" ]] || usage

mkdir -p "$output_dir"
work_dir=$(mktemp -d /tmp/sse_snapshot_gru_package_XXXXXX)
cleanup() { rm -rf "$work_dir"; }
trap cleanup EXIT

handoff_root="$handoff"
if [[ -f "$handoff" ]]; then
  handoff_root="$work_dir/handoff"
  mkdir -p "$handoff_root"
  case "$handoff" in
    *.tar.zst)
      zstd_bin=${ZSTD_BIN:-$(command -v zstd || true)}
      if [[ -z "$zstd_bin" && -x /opt/miniconda3/bin/zstd ]]; then
        zstd_bin=/opt/miniconda3/bin/zstd
      fi
      [[ -n "$zstd_bin" ]] || { echo "zstd is required for archive handoff" >&2; exit 1; }
      # If a handoff has an incomplete provenance tail, continue after tar's
      # EOF and verify every deployment-critical file below.
      set +e
      "$zstd_bin" -dc "$handoff" 2>/dev/null | tar -xf - -C "$handoff_root" 2>/dev/null
      set -e
      top=$(find "$handoff_root" -mindepth 1 -maxdepth 1 -type d -print -quit)
      [[ -n "$top" ]] || { echo "handoff archive has no root directory" >&2; exit 1; }
      handoff_root="$top"
      ;;
    *) echo "unsupported handoff archive; expected .tar.zst or directory" >&2; exit 1 ;;
  esac
fi

required=(
  "$handoff_root/models/baseline/best.pt"
  "$handoff_root/models/baseline/baseline.json"
  "$handoff_root/models/auction59/best.pt"
  "$handoff_root/models/auction59/auction59.json"
)
for path in "${required[@]}"; do
  [[ -f "$path" ]] || { echo "missing handoff file: $path" >&2; exit 1; }
done

stage="$work_dir/$package_id"
mkdir -p "$stage/models" "$stage/config" "$stage/tools" "$stage/source"
cp "$repo_dir/sse/model/sse_snapshot_gru_contract.json" "$stage/config/"
cp "$repo_dir/sse/model/sse_snapshot_gru_routing.json" "$stage/config/"
cp "$repo_dir/tools/model/convert_snapshot_gru.py" "$stage/tools/"
(cd "$repo_dir" && cp --parents common/model/snapshot_gru/snapshot_gru_runtime.h common/model/snapshot_gru/snapshot_gru_runtime.cpp sse/model/snapshot_ensemble.h sse/model/snapshot_ensemble.cpp "$stage/source/")
cp -R "$repo_dir/third_party/eigen3" "$stage/source/third_party/"
mkdir -p "$stage/source/third_party/nlohmann"
cp "$repo_dir/third_party/nlohmann/json.hpp" "$stage/source/third_party/nlohmann/"
cp "$handoff_root/models/baseline/baseline.json" "$stage/models/"
cp "$handoff_root/models/auction59/auction59.json" "$stage/models/"

python_bin=${PYTHON_BIN:-/usr/bin/python3.6m}
[[ -x "$python_bin" ]] || python_bin=$(command -v python3)
"$python_bin" "$stage/tools/convert_snapshot_gru.py" \
  --checkpoint "$handoff_root/models/baseline/best.pt" \
  --scaler "$stage/models/baseline.json" --feature-count 36 \
  --output "$stage/models/baseline.ssegru"
"$python_bin" "$stage/tools/convert_snapshot_gru.py" \
  --checkpoint "$handoff_root/models/auction59/best.pt" \
  --scaler "$stage/models/auction59.json" --feature-count 95 \
  --output "$stage/models/auction59.ssegru"

(cd "$stage" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tarball="$output_dir/${package_id}.tar.gz"
tar -czf "$tarball" -C "$work_dir" "$package_id"
sha256sum "$tarball"
echo "wrote $tarball"
