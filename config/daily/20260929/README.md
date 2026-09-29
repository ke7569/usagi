# 2026-09-29 execution daily configs

These are copies of the existing Guoxin SH/SZ daily inputs with only the
requested position targets changed. The source files under `/home/pta/live_config`
remain untouched.

| Market | Symbol | Pre-execution shares | `external_delta` | Target `static_position` |
| --- | --- | ---: | ---: | ---: |
| SH | 600089.SH | 0 | +200 | 200 |
| SH | 600633.SH | 200 | -200 | 0 |
| SH | 600859.SH | 200 | -200 | 0 |
| SZ | 300703.SZ | 0 | +200 | 200 |

The daily files preserve all other instruments and market data. Their
`static_data_hash` fields have been recomputed from the complete `ins_params`
objects. The copied SH manifest records its source config SHA256 and contains
the new SHA256 for the adjusted SH config; its stock-day CSV checksum still
refers to `/home/pta/live_config/guoxin-sh/stock_day_20260929.csv`.

The dated daily file must replace the corresponding daily input in a deployment
package before that day's runtime is prepared. These files alone do not enable
live trading: the production runtime, TD plugin, account reconciliation, and
deployment gates retain their own requirements.
