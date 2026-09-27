#!/usr/bin/env bash
set -euo pipefail
# Restore the previous launcher for the next start. Do not switch running
# strategies while their orders might still be outstanding.
OVERRIDE=/etc/systemd/system/sze-trade.service.d/20-v06.conf
if systemctl is-active --quiet sze-trade.service; then
  echo 'trade is active; stop/reconcile current orders before rollback' >&2
  exit 1
fi
if [[ -f "$OVERRIDE" ]]; then
  mv "$OVERRIDE" "/home/zane/releases/pre-v06-20260910/20-v06.disabled.$(date +%Y%m%dT%H%M%S)"
fi
systemctl daemon-reload
echo 'Old trade launcher restored for next start; capture was not touched.'
