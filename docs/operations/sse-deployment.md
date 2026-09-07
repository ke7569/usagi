# SSE live capture package

This package is for tomorrow's first live connection only. It receives raw SSE
FPGA UDP datagrams from the snapshot and tick channels, records local arrival
timestamps, and does not start a strategy or connect TD.

## Upload contents

Upload the generated `sse-live-capture-*.tar.gz` as one file. It contains:

```text
bin/sse_udp_observer
config/sse_udp_observer.example.json
config/sse_udp_observer_dongguan.example.json
docs/LV1_ENDPOINTS.md
start_sse_capture.sh
stop_sse_capture.sh
analyze_sse_capture.py
SHA256SUMS
```

## Start

On the live server, choose the actual market-data NIC IP. For Jinqiao:

```text
./start_sse_capture.sh --site jinqiao --interface-ip MARKET_DATA_NIC_IP --output-dir ./capture
```

For Dongguan:

```text
./start_sse_capture.sh --site dongguan --interface-ip MARKET_DATA_NIC_IP --output-dir ./capture
```

The physical server IPs in the supplied sheet are reference values and may not
be the multicast-facing interface. Replace `MARKET_DATA_NIC_IP` with the
actual address. The command prints the JSONL and
stderr paths. Keep the process running for the full session.

Stop after the session:

```text
./stop_sse_capture.sh ./capture
```

Analyze the relationship between snapshot and tick arrivals:

```text
./analyze_sse_capture.py ./capture/sse_udp_*.jsonl
```

The analyzer accepts one or more JSONL files, so a set of rotated files can be
passed with the same command.

Each JSONL row includes `ts_ns` (Linux `SO_TIMESTAMPNS` wall-clock Unix
nanoseconds, with a user-space fallback),
`monotonic_ns` (local monotonic nanoseconds), channel name, source endpoint,
length, and the first 64 bytes. `monotonic_ns` is the preferred field for
within-process timing; `ts_ns` is retained for correlation with server logs.

The channel names `snapshot`/`snapshot_primary` and `tick`/`tick_primary`
correspond to the two streams requested for tomorrow: snapshot (切片/快照)
and tick-by-tick (逐笔) FPGA market data.

Do not infer packet field layouts from this capture alone. Preserve the raw
JSONL and stderr log for the next decoder step.

## 2026-08-18 prediction candidate

The repository also contains a prediction-only candidate configuration:

```text
config/examples/sse/main_sse_prediction_20260818.conf
config/examples/sse/sse_fpga_md_prediction_20260818.json
config/examples/sse/config_sse_hybrid_prediction_20260818.json
```

It enables Snapshot and tick market data, uses the native hybrid model, applies
the strict one-shot `>100us` SSE batch policy, writes prediction diagnostics,
and has no TD source. Before startup, the target host must set the network
interface, install the hybrid model directory at the configured path, connect
the vendor SSE decoder/frozen factor adapter, and load positive daily turnover
thresholds for the decoded SSE universe. `production_approval` remains false.

The live adapter must follow [the SSE sampling contract](../contracts/sse-sampling.md). A fixed
100us periodic publisher is not contract-compatible.

## Shanghai ATP TD plugin

The repository can build the Shanghai ATP adapter from the shared Guoxin ATP
source with the exchange-specific identity below:

```bash
cmake -S . -B build/sse-td \
  -DSSE_BUILD_TD=ON -DSZE_BUILD_TD=OFF \
  -DSZE_TD_API_DIR=/path/to/adapters/td/atp/api \
  -DRUNTIME=/path/to/runtime_so -DCMAKE_BUILD_TYPE=Release
cmake --build build/sse-td --target sse_td -j2
```

This produces `libsse_td.so` with source id `190`, engine key `sse_td`, and
ATP market id `101` (SSE). It is the same ATP adapter source used for the SZE
plugin, with the market resolver mapping `SSE`/`SH` to market 101; do not
rename `libsze_td.so` and treat it as a Shanghai binary.

The query-test template is `config/examples/sse/sse_td_query.example.json`. It keeps
`startup_cancel_all_orders=false`. Replace both IX endpoints and all account
fields independently. In particular, `cust_id`, `fund_account_id`, and
`account_id` are different fields and must not be filled by copying one value
without vendor confirmation. The supplied server document contains network
and machine information but does not contain these two IX IPs or the account
credentials, so this repository intentionally leaves them as placeholders.
For a TD-only query process with no strategy loaded, use
`config/examples/sse/main_sse_td_query.example.conf`.

The transfer directory
`build/configs/sse-deepwin-deps-20260819/` contains the ATP headers,
`libsse_td.so`, and the Python 3.6/Boost 1.62/ZMQ/log4cplus/Deepwin runtime
libraries. Verify its `SHA256SUMS` before copying it to the target host.

## Snapshot GRU model package

The handoff's SSE opening model is packaged separately from the raw capture
bundle. It contains the two native `SSESGRU1` artifacts, their normalizers,
feature/routing contracts, the standard-library-only converter, and the C++
serving sources:

```bash
deploy/sse/build_snapshot_gru_package.sh \
  --handoff /path/to/hermespro-sse-opening-gru-auction59-ensemble-slim-v1-20260817 \
  --output-dir build/configs/sse-snapshot-gru-20260817
```

An archive handoff (`.tar.zst`) is also accepted. The script verifies all four
deployment-critical model/scaler files after extraction and records
`SHA256SUMS`. The package remains a candidate artifact
(`production_approval` is false); it is intended for offline parity and later
strategy integration.

## Combined Snapshot and tick model package

Build the complete dual-stream model handoff with the forced 09:35 switch:

```bash
deploy/sse/build_hybrid_model_package.sh \
  --tick-handoff /home/external/v04-legacy-midmix-sse.tar.zst \
  --snapshot-handoff /home/external/hermespro-sse-opening-gru-auction59-ensemble-slim-v1-20260817.tar.zst \
  --output-dir build/configs/sse-hybrid-model-20260817
```

The output contains the 50-factor `SSEMODL1` tick artifact, both Snapshot
`SSESGRU1` artifacts, normalizers, factor/sampling contracts, hybrid routing,
dated prediction configs, live batch sampler, native C++ sources, conversion
tools, and `SHA256SUMS`. The tick model must be run before 09:35 to warm its
recurrent state even though only Snapshot output is selected during the
opening five-minute window.
