# Trading Gateway Integration Note

## Current finding

The live Shenzhen deployment was previously capture/recovery/prediction shadow
only. The independent source tree now contains the Shenzhen TD adapter and a
live test-order configuration, while account credentials and the vendor ATP
runtime remain deployment-only inputs.

The adapter is built as `libsze_td.so` from the shared `TDEngineGXBSE`
implementation with `T0_TD_ENGINE_KEY="sze_td"` and default market id `102`.
The strategy reaches it through the normal Deepwin `ITDEngine` callbacks using
`td_source_index=[180]`.

## Target account

The requested target is the Yuheng neutral fund account under `branch_id=1700`.
Credentials and account numbers are intentionally not stored in this project.
They must be supplied through a root-only deployment secret.

## Endpoint roles

- Shenzhen ordinary spot: two failover endpoints, port 32001
- Shenzhen spot IX: two failover endpoints, port 32002
- Shanghai spot IX: two failover endpoints, port 32002
- The Yuheng account uses the spot IX path, with Shenzhen/Shanghai allocation
  0.5/0.5 and Shanghai as the default deposit/withdrawal node.

## Required implementation before activation

1. Supply the broker ATP Quant runtime `libatpquantapi.so` and confirm its ABI.
2. Confirm that the plugin is loaded as the single Shenzhen `vtd` source 180.
3. Use the existing endpoint failover, login, heartbeat, reconnect, query,
   order, cancel, and response-correlation implementation.
4. Add a root-only secret file containing account credentials; do not put them
   into JSON committed to Git or into process command lines.
5. Start with `sze_test_order.enabled=true` for one 100-share FAK order and
   delayed cancel; keep recovery/capture configs order-disabled.
6. Record the exact TD source id, plugin SHA256, endpoint configuration hash,
   and account alias in startup logs without printing secrets.
