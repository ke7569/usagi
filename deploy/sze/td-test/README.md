# Shenzhen TD Test Deployment

This directory contains the Shenzhen strategy and TD plugin built from the
same source commit:

- `libt0_strategy_sze.so`
- `libsze_td.so`
- `config_sze_td_test_000001.json`
- `main_sze_td_test_000001.conf`
- `deepwin_sze_td.example.json`

The deployment host must already provide the Deepwin runtime libraries and the
broker ATP Quant SDK. The SDK library `libatpquantapi.so` is intentionally not
included in the public repository due to vendor redistribution restrictions.

Before enabling the test order, copy the account-specific TD configuration to
the runtime configuration location and replace all `REPLACE_*` placeholders.
