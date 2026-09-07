#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${USAGI_BUILD_DIR:-${ROOT}/build/dev}"
OUT_DIR="${USAGI_PACKAGE_DIR:-${ROOT}/build/packages}"
STAMP="${SSE_PACKAGE_STAMP:-$(date -u +%Y%m%d_%H%M%S)}"
STAGE="${OUT_DIR}/sse-live-capture-${STAMP}"
ARCHIVE="${OUT_DIR}/sse-live-capture-${STAMP}.tar.gz"

[[ -x "${BUILD_DIR}/sse_udp_observer" ]] || {
  echo "missing ${BUILD_DIR}/sse_udp_observer; build it first in the CentOS 7.9 builder" >&2
  exit 3
}
mkdir -p "${OUT_DIR}"
if [[ -e "${STAGE}" || -e "${ARCHIVE}" ]]; then
  echo "package output already exists: ${STAGE} or ${ARCHIVE}" >&2
  exit 4
fi
mkdir -p "${STAGE}/bin" "${STAGE}/config" "${STAGE}/docs"
cp "${BUILD_DIR}/sse_udp_observer" "${STAGE}/bin/"
cp "${ROOT}/config/examples/sse/sse_udp_observer.example.json" "${STAGE}/config/"
cp "${ROOT}/config/examples/sse/sse_udp_observer_dongguan.example.json" "${STAGE}/config/"
cp "${ROOT}/docs/operations/sse-lv1-endpoints.md" "${STAGE}/docs/"
cp "${ROOT}/deploy/sse/start_sse_capture.sh" "${STAGE}/"
cp "${ROOT}/deploy/sse/stop_sse_capture.sh" "${STAGE}/"
cp "${ROOT}/deploy/sse/analyze_sse_capture.py" "${STAGE}/"
cp "${ROOT}/docs/operations/sse-deployment.md" "${STAGE}/README.md"
chmod 0755 "${STAGE}/bin/sse_udp_observer" "${STAGE}/"*.sh "${STAGE}/analyze_sse_capture.py"
WORKTREE_STATUS="$(cd "${ROOT}" && git status --porcelain)"
{
  echo "package=sse-live-capture"
  echo "created_utc=${STAMP}"
  echo "git_commit=$(cd "${ROOT}" && git rev-parse HEAD)"
  echo "git_branch=$(cd "${ROOT}" && git symbolic-ref --short HEAD 2>/dev/null || true)"
  if [[ -n "${WORKTREE_STATUS}" ]]; then
    echo "git_dirty=true"
  else
    echo "git_dirty=false"
  fi
  echo "source_layout=usagi"
  echo "builder=centos-7.9-gcc-4.8.5"
  echo "scope=raw SSE snapshot/tick UDP capture only"
} > "${STAGE}/PACKAGE_INFO"
(cd "${STAGE}" && find . -type f ! -path './SHA256SUMS' -print0 | sort -z | xargs -0 sha256sum) > "${STAGE}/SHA256SUMS"
tar -C "${OUT_DIR}" -czf "${ARCHIVE}" "$(basename "${STAGE}")"
sha256sum "${ARCHIVE}" > "${ARCHIVE}.sha256"
echo "archive=${ARCHIVE}"
echo "sha256=$(awk '{print $1}' "${ARCHIVE}.sha256")"
