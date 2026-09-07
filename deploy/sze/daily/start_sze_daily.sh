#!/usr/bin/env bash
set -euo pipefail

DAY="${TRADING_DAY:-$(date +%Y%m%d)}"
RUNTIME_ROOT="${SZE_RUNTIME_ROOT:-/run/sze}"
READY_FILE="${RUNTIME_ROOT}/${DAY}/strategy/recovery.ready"
rm -f "$READY_FILE"

systemctl start sze-recovery.service
for _ in $(seq 1 120); do
  systemctl is-active --quiet sze-recovery.service || {
    echo "recovery service exited before readiness" >&2
    exit 1
  }
  [[ -f "$READY_FILE" ]] && break
  sleep 1
done
[[ -f "$READY_FILE" ]] || { echo "recovery readiness timeout" >&2; exit 1; }
systemctl start sze-trade.service
systemctl is-active --quiet sze-trade.service || {
  echo "trade service failed to start" >&2
  exit 1
}
echo "SZE services started trading_day=${DAY} recovery_ready=1 trade_active=1"
