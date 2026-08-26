# Shanghai TD Test Order

The SSE TD plugin is built as `libsse_td.so` and loaded under engine key
`sse_td` with TD source `190`.  It shares the ATP request/reply state machine
with `libsze_td.so`, but uses the SSE market mapping (`SSE`/`SH`) and must be
given the broker-confirmed Shanghai `market_id` in the account configuration.

The first live acceptance should be a single, explicitly authorized order:

- instrument: `600519.SH` (or another user-approved Shanghai stock)
- side: buy or sell only as explicitly approved
- quantity: one valid Shanghai lot, normally 100 shares for an ordinary stock
- price: a user-approved limit price; do not use a guessed market order
- time condition: the broker-approved IOC/FAK equivalent, if supported
- cancel: delayed cancel only after the order response has supplied an order id

The production strategy must route to TD source `190`.  Keep the existing SSE
prediction-only config unchanged; use the dedicated TD test config and a
root-only account secret.  Do not place credentials in JSON committed to the
repository or in a process command line.

Before enabling a real order, verify all of the following in the vendor SDK
and broker environment:

1. `libatpquantapi.so` and matching `atp_quant_api.h` are installed.
2. The Shanghai endpoint list and TCP-direct setting are correct.
3. The ATP Shanghai `market_id`, ordinary-stock `business_type`, side/order
   type, lot size, and FAK/cancel rules are confirmed.
4. The account has enough cash/stock and the maximum order amount is bounded.
5. Startup cancellation and position synchronization are enabled for the test.

Expected evidence is a successful login, account/position query, one order
response, an order return, and either a controlled fill or a confirmed cancel.
The plugin logs request/reply and first-return latency without logging secrets.

## Build gate

From the complete `sze-t0` source tree, after installing the matching vendor
dependencies, run:

```bash
SSE_BUILD_TD=ON SZE_TD_API_DIR=/path/to/atp-sdk ./build_sze.sh
```

The resulting `libsse_td.so` must be copied together with the exact matching
Deepwin runtime.  The current Jinqiao host fails the environment gate because
all seven required SDK/runtime files are absent; it is not a compiler failure.
