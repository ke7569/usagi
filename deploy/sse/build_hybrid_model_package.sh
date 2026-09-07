#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 --tick-handoff DIR_OR_TAR_ZST --snapshot-handoff DIR_OR_TAR_ZST --output-dir DIR [--package-id ID]" >&2
  exit 2
}

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
tick_handoff=""
snapshot_handoff=""
output_dir=""
package_id="sse-hybrid-model-20260817"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --tick-handoff) tick_handoff=${2:-}; shift 2 ;;
    --snapshot-handoff) snapshot_handoff=${2:-}; shift 2 ;;
    --output-dir) output_dir=${2:-}; shift 2 ;;
    --package-id) package_id=${2:-}; shift 2 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done
[[ -n "$tick_handoff" && -n "$snapshot_handoff" && -n "$output_dir" ]] || usage

mkdir -p "$output_dir"
work_dir=$(mktemp -d /tmp/sse_hybrid_model_package_XXXXXX)
cleanup() { rm -rf "$work_dir"; }
trap cleanup EXIT

extract_handoff() {
  local source=$1
  local destination=$2
  if [[ -d "$source" ]]; then
    printf '%s\n' "$source"
    return
  fi
  [[ -f "$source" ]] || { echo "handoff does not exist: $source" >&2; exit 1; }
  mkdir -p "$destination"
  case "$source" in
    *.tar.zst)
      local zstd_bin=${ZSTD_BIN:-$(command -v zstd || true)}
      if [[ -z "$zstd_bin" && -x /opt/miniconda3/bin/zstd ]]; then
        zstd_bin=/opt/miniconda3/bin/zstd
      fi
      [[ -n "$zstd_bin" ]] || { echo "zstd is required for archive handoff" >&2; exit 1; }
      "$zstd_bin" -dc "$source" | tar -xf - -C "$destination"
      find "$destination" -mindepth 1 -maxdepth 1 -type d -print -quit
      ;;
    *) echo "unsupported handoff; expected directory or .tar.zst" >&2; exit 1 ;;
  esac
}

tick_root=$(extract_handoff "$tick_handoff" "$work_dir/tick")
snapshot_root=$(extract_handoff "$snapshot_handoff" "$work_dir/snapshot")
[[ -f "$tick_root/model/state_dict.npz" ]] || { echo "tick state_dict.npz is missing" >&2; exit 1; }
[[ -f "$snapshot_root/models/baseline/best.pt" ]] || { echo "snapshot baseline best.pt is missing" >&2; exit 1; }
[[ -f "$snapshot_root/models/auction59/best.pt" ]] || { echo "snapshot Auction59 best.pt is missing" >&2; exit 1; }

stage="$work_dir/$package_id"
mkdir -p "$stage/models/tick" "$stage/models/snapshot" "$stage/config" "$stage/tools" \
  "$stage/source"
cp "$repo_dir/sse/model/sse_hybrid_routing.json" "$stage/config/"
cp "$repo_dir/sse/model/sse_snapshot_gru_contract.json" "$stage/config/"
cp "$repo_dir/sse/model/sse_snapshot_gru_routing.json" "$stage/config/"
cp "$repo_dir/config/examples/sse/config_sse_hybrid_prediction_20260818.json" "$stage/config/"
cp "$repo_dir/config/examples/sse/sse_fpga_md_prediction_20260818.json" "$stage/config/"
cp "$repo_dir/config/examples/sse/main_sse_prediction_20260818.conf" "$stage/config/"
cp "$tick_root/factors/factor_contract.json" "$stage/config/tick_factor_contract.json"
cp "$tick_root/factors/sampling_config.json" "$stage/config/tick_sampling_config.json"
cp "$tick_root/factors/factors.txt" "$stage/config/tick_factors.txt"
cp "$tick_root/model/architecture.json" "$stage/config/tick_architecture.json"
cp "$tick_root/golden/tolerances.json" "$stage/config/tick_tolerances.json"
cp "$snapshot_root/models/baseline/baseline.json" "$stage/models/snapshot/"
cp "$snapshot_root/models/auction59/auction59.json" "$stage/models/snapshot/"

cp "$repo_dir/tools/model/convert_state_dict_npz.py" "$stage/tools/"
cp "$repo_dir/tools/model/convert_snapshot_gru.py" "$stage/tools/"
(cd "$repo_dir" && cp --parents common/model/legacy_midmix/sse_model_runtime.h common/model/legacy_midmix/sse_model_runtime.cpp \
  common/model/snapshot_gru/snapshot_gru_runtime.h common/model/snapshot_gru/snapshot_gru_runtime.cpp \
  sse/model/snapshot_ensemble.h sse/model/snapshot_ensemble.cpp \
  sse/model/sse_hybrid_model.h sse/model/sse_hybrid_model.cpp \
  sse/sampling/sse_batch_end_sampler.h sse/sampling/sse_batch_end_sampler.cpp "$stage/source/")
cp -R "$repo_dir/third_party/eigen3" "$stage/source/third_party/"
mkdir -p "$stage/source/third_party/nlohmann"
cp "$repo_dir/third_party/nlohmann/json.hpp" "$stage/source/third_party/nlohmann/"

python_bin=${PYTHON_BIN:-/usr/bin/python3.6m}
[[ -x "$python_bin" ]] || python_bin=$(command -v python3)
"$python_bin" "$stage/tools/convert_state_dict_npz.py" \
  "$tick_root/model/state_dict.npz" "$stage/models/tick/ssemodl1.bin"
"$python_bin" "$stage/tools/convert_snapshot_gru.py" \
  --checkpoint "$snapshot_root/models/baseline/best.pt" \
  --scaler "$stage/models/snapshot/baseline.json" --feature-count 36 \
  --output "$stage/models/snapshot/baseline.ssegru"
"$python_bin" "$stage/tools/convert_snapshot_gru.py" \
  --checkpoint "$snapshot_root/models/auction59/best.pt" \
  --scaler "$stage/models/snapshot/auction59.json" --feature-count 95 \
  --output "$stage/models/snapshot/auction59.ssegru"

(cd "$stage" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tarball="$output_dir/${package_id}.tar.gz"
tar -czf "$tarball" -C "$work_dir" "$package_id"
sha256sum "$tarball"
echo "wrote $tarball"
