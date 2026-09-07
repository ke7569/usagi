# Shenzhen fixed-system deployment

This deployment has two source configuration layers:

```text
/home/zane/configs/
  general_config/
    sze_system.json
    td_credentials.json       # mode 0600, never copied into manifests/logs
  config_sze_daily_YYYYMMDD.json
  current -> config_sze_daily_YYYYMMDD.json
```

`sze_system.json` is fixed. It owns multicast/NIC settings, CPU placement,
shard count, journal and SHM layout, model identity, sessions, output policy,
prediction parameters, and trade/risk parameters. Do not date this file.
At TD launch, `td_receive_cpu` and `td_send_cpu` are injected into both the
engine-level and account-level ATP configuration, so private credentials
cannot silently override the fixed CPU plan.
Recovery-only shard configs are generated as `hp-shadow` with
`capture_only=true`, because the strategy guard correctly forbids realtime
routing without a TD source. The separate trade config is `hp-realtime`, has
`trading_enabled=true`, and is the only generated config that can route orders.

The one daily JSON contains only:

```text
trading_day
static_data_source_date
static_data_hash
ins_params
```

`static_data_hash` is SHA256 over canonical compact JSON for `ins_params`
(`sort_keys=True`, separators `,` and `:`, ASCII encoding). A symbol is in the
trade universe exactly when its `static_position` is nonzero. There is no
separate daily trade-symbol list. Daily `ins_params` must not contain `cpu` or
`last_position`: CPU ownership is fixed and broker position is queried by TD;
the generated compatibility strategy config initializes `last_position` to
zero before that broker state arrives.

## Runtime flow

At 08:50, `sze-capture.timer` starts capture from the fixed system config. A
missing daily config does not block capture. The generated Deepwin and main
configs are stored under `/run/sze/YYYYMMDD/capture`.

At 09:05, `sze-start.timer` validates the exact-date daily config, model hash,
static-data hash, CPU ownership, and private credential permissions. It then
generates recovery shard and trade files under
`/run/sze/YYYYMMDD/strategy`. The generated files are runtime artifacts, not
inputs and not research-server deliverables. Recovery/trade never select the
"newest" config and never silently use yesterday's static data.

At 15:01, `sze-stop.timer` stops trade, recovery, and capture in that order.
The host timezone must remain `Asia/Shanghai`; the timer files intentionally
use host-local wall clock syntax for compatibility with the production
systemd version. The old systemd cannot parse weekday ranges, so timers fire
daily. On holidays, capture may remain available while the absent exact-date
daily config prevents prediction and trading.

## Missing daily policy

- Capture starts and journals the full feed.
- Recovery/prediction wait in failed preflight and alert.
- TD does not start and no order can be submitted.
- A stale daily file is rejected even if `/home/zane/configs/current` points to it.
- Legacy all-in-one daily files are rejected by production preflight. Convert
  them once on an offline machine:

```bash
python3 migrate_legacy_daily.py old_config.json \
  config_sze_daily_YYYYMMDD.json \
  --trading-day YYYYMMDD --source-date YYYYMMDD
```

## Installation

Install fixed files once:

```bash
install -m 0644 sze_system.json /home/zane/configs/general_config/sze_system.json
install -m 0755 prepare_sze_runtime.py /home/zane/run_main/prepare_sze_runtime.py
install -m 0755 migrate_legacy_daily.py /home/zane/run_main/migrate_legacy_daily.py
install -m 0755 merge_sze_td_runtime.py /home/zane/run_main/merge_sze_td_runtime.py
install -m 0755 run_sze_capture_daily.sh /home/zane/run_main/run_sze_capture_daily.sh
install -m 0755 run_sze_recovery_launcher.sh /home/zane/run_main/run_sze_recovery_launcher.sh
install -m 0755 run_sze_trade_daily.sh /home/zane/run_main/run_sze_trade_daily.sh
install -m 0755 start_sze_daily.sh /home/zane/run_main/start_sze_daily.sh
install -m 0755 sze_daily_preflight.sh /home/zane/run_main/sze_daily_preflight.sh
```

Install the fixed services/timers into `/etc/systemd/system`, run
`systemctl daemon-reload`, and enable only:

```bash
systemctl enable sze-capture.timer sze-start.timer sze-stop.timer
```

Before production cutover, run a non-trading dry run and verify generated
configs, process affinity, journal continuity, recovery lag, predictions, TD
login/account queries, and a real-NIC latency comparison. The direct sharded
ring prototype remains disabled until that separate validation is complete.
The start coordinator waits for an atomic `recovery.ready` marker written only
after every recovery worker survives its startup check; TD cannot race runtime
generation.

The direct sharded-ring prototype is not part of the production `libsze_md.so`.
To build its standalone test/benchmark explicitly, configure with
`-DSZE_BUILD_DIRECT_SHARDED_RING=ON`.

## Current HA boundary

Only one physical host receives Shenzhen multicast, so this is not true HA.
Keep one authoritative low-latency capture. A same-host raw UDP recorder may
be added only after OpenOnload multicast duplication and latency are measured.
Closed journal segments should be copied asynchronously to research storage
with SHA256 verification. Any capture restart after a sequence gap must mark
the day discontinuous and prohibit new trading risk.
