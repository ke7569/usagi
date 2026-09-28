#!/usr/bin/env bash
set -euo pipefail
DAY="${TRADING_DAY:-$(date +%Y%m%d)}"
SOURCE=/home/zane/usagi-sze-v06-20260910
RELEASE=/home/zane/releases/sze-v06-live-20260928-book-guard
RUNTIME=/run/sze-v06/$DAY/trade
ORIGINAL=/run/sze/$DAY/strategy/trade
SYSTEM=/home/zane/configs/general_config/sze_system.json
[[ "$DAY" =~ ^[0-9]{8}$ ]] || exit 2
python3 "$SOURCE/deploy/sze/v06/prepare_v06_trade.py" \
  --day "$DAY" --original "$ORIGINAL" --release "$RELEASE" --output "$RUNTIME"
IFS= read -r CREDENTIALS < <(python3 -c \
  'import json,sys; print(json.load(open(sys.argv[1]))["trade"]["credentials_path"])' "$SYSTEM")
[[ -f "$CREDENTIALS" && "$(stat -c %a "$CREDENTIALS")" == 600 ]] || exit 1
python3 /home/zane/run_main/merge_sze_td_runtime.py \
  "/run/sze/$DAY/capture/deepwin.json" "$CREDENTIALS" "$SYSTEM" "$RUNTIME/deepwin.json"
install -m 0600 "$RUNTIME/deepwin.json" /opt/deepwin/master/etc/deepwin/deepwin.json
export TZ=Asia/Shanghai
export LD_LIBRARY_PATH="$RELEASE:/home/zane/run_main:/home/zane/lib:/home/zane/runtime_so/deepwin_core/lib/wingchun:/home/zane/runtime_so/deepwin_core/lib/yijinjing:/home/zane/runtime_so/third_party/wingchun:/home/zane/runtime_so/third_party/boost:${LD_LIBRARY_PATH:-}"
exec /home/zane/bin/main "$RUNTIME/main.conf"
