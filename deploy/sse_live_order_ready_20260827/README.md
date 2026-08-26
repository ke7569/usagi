# SSE live-order preparation bundle

This directory is an isolated build/deployment artifact. It is not installed
over the current `/home/zane/sse` prediction process.

The strategy uses the same `ZStrategy` order decisions as SZE. For Shanghai,
`ZStrategy` routes orders to `ExchangeID=SSE` and TD source `190`; the TD plugin
uses market id `101`. Snapshot and tick recurrent states remain independent;
Snapshot is selected for `[09:30,09:35)`, while tick is warmed from the first
accepted event and selected from `09:35` onward. Snapshot generation continues
to `09:40` for diagnostics.

Before starting, generate `configs/config_sse_live.json` from the daily static
JSON with `prepare_sse_live_config.py`, then copy the broker-confirmed account
secret to `configs/deepwin.json` with mode `0600`. Never commit that secret
file. The default generated config has `production_approval=false`;
the strategy will log predictions and risk state but will block all orders.

The supplied `main_sse_t0.conf` references `libsse_md.so`, which must be the
broker-provisioned SSE FPGA MD plugin. This bundle supplies only the newly built
`libt0_strategy_sse.so` and `libsse_td.so`.

The launcher performs a preflight only after the production environment variable is
enabled: `/home/zane/bin/main`, `libsse_md.so`, both built plugins, account secret,
and all shared-library dependencies must be present. A missing runtime artifact exits
with code 3 before Deepwin is started.
The actual order gate requires all of:

* `trading_enabled=true`;
* `sse_order_routing.enabled=true` and `mode=live`;
* `production_approval=true`;
* account and every configured position query ready; and
* `SSE_ENABLE_LIVE_ORDER=YES` for the launcher.

Do not enable these until a human has approved the instrument, side, price,
quantity and cancellation policy. The test-order block remains disabled by
default.
