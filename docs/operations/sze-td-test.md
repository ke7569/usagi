# Shenzhen Test Order TD

The strategy and TD path are separate layers:

1. `ZStrategy` calls the standard Deepwin `insert_limit_order` and
   `cancel_order` callbacks using `td_source_index[0]`.
2. `libsze_td.so` loads `TDEngineGXBSE` under engine key `sze_td`, maps
   `SZE/SZ` to ATP market id `102`, and forwards order, cancel, order-return,
   trade-return, position-query, and startup-cancel requests through the ATP
   Quant API.

For a controlled connectivity test, add the following object to a live
strategy config:

```json
"sze_test_order": {
  "enabled": true,
  "instrument": "000001.SZ",
  "side": "buy",
  "price": 0.0,
  "volume": 100,
  "trigger_after_signals": 1,
  "cancel_delay_ms": 1000
}
```

The trigger fires once, after the warm-up signals and the first valid signal
for the selected instrument. A zero price uses the current opposite best
price. The order is sent as FAK and is cancelled after the configured delay.
The test is rejected by config validation unless routing mode is `live`.
The default examples keep it disabled.

Before enabling it on a real account, verify the account, instrument static
data, lot size, and TD endpoint. Confirm logs for `[SZTestOrder] submitted`,
`on_rsp_order_insert`, `[OrderRtn]`, `[TradeRtn]` (if filled), and the delayed
cancel response.
