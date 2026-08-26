# Shanghai TD Test Deployment

This deployment is intentionally separate from the SSE prediction-only run.
It expects the following files in one Deepwin runtime directory:

- `libsse_td.so`
- `libt0_strategy_sse.so` with live TD routing support
- `config_sse_td_test_600519.json`
- `main_sse_td_test_600519.conf`
- a root-only account configuration derived from
  `deepwin_sse_td.example.json`

The TD source id is `190`.  The plugin requires a broker ATP Quant SDK and the
Deepwin headers/libraries at build and run time; neither is redistributed by
this repository.  Replace every `REPLACE_*` value in the secret copy only.
Do not enable the order until the Shanghai market id, business type, endpoint,
lot size, price, and cancellation policy have been confirmed by the broker.

Run `check_sse_td_environment.sh` first.  It must report
`READY_FOR_BUILD`; an incomplete environment is a hard stop and must not be
worked around with guessed libraries or market parameters.
