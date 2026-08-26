#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="/home/zane/td_query_runtime_20260824/lib:${ROOT}:${LD_LIBRARY_PATH:-}"

if [[ ! -f "${ROOT}/configs/config_sse_live.json" ]]; then
  echo "missing generated strategy config" >&2
  exit 2
fi
if [[ ! -f "${ROOT}/configs/deepwin.json" ]]; then
  echo "missing root-only Deepwin TD account config" >&2
  exit 2
fi
chmod 600 "${ROOT}/configs/deepwin.json"

# Production is intentionally opt-in.  The strategy itself also enforces the
# production_approval gate, so a stale launcher variable cannot place orders.
if [[ "${SSE_ENABLE_LIVE_ORDER:-NO}" != "YES" ]]; then
  echo "SSE staged launcher: gate is OFF; refusing to start Deepwin"
  exit 0
fi
required=("/home/zane/bin/main" "${ROOT}/libsse_md.so" "${ROOT}/libt0_strategy_sse.so" "${ROOT}/libsse_td.so" "${ROOT}/configs/deepwin.json")
for path in "${required[@]}"; do
  if [[ ! -r "${path}" ]]; then
    echo "SSE staged launcher: missing runtime artifact ${path}" >&2
    exit 3
  fi
done
if ldd "${ROOT}/libt0_strategy_sse.so" 2>/dev/null | grep -q "not found"; then
  echo "SSE staged launcher: libt0_strategy_sse.so has unresolved shared-library dependencies" >&2
  ldd "${ROOT}/libt0_strategy_sse.so" >&2 || true
  exit 3
fi
if ldd "${ROOT}/libsse_td.so" 2>/dev/null | grep -q "not found"; then
  echo "SSE staged launcher: libsse_td.so has unresolved shared-library dependencies" >&2
  ldd "${ROOT}/libsse_td.so" >&2 || true
  exit 3
fi
exec /home/zane/bin/main "${ROOT}/main_sse_t0.conf"
