# SSE UDP Market Observer

This diagnostic tool observes the Shanghai feed without decoding or forwarding it to Deepwin. It uses the reusable channel runtime in `common/stream/UdpChannelRuntime.cpp`; each configured channel owns one UDP multicast socket and records one JSONL row per datagram:

- kernel wall-clock receive timestamp (`ts_ns`)
- local monotonic receive timestamp (`monotonic_ns`)
- configured channel name
- sender IP/port
- datagram length
- first 64 bytes as hexadecimal (`prefix_hex`)

Run only on the provisioned host after confirming the multicast interface and channel permissions:

```text
./sse_udp_observer /tmp/sse-feed.jsonl snapshot 239.35.80.5 37105 tick 239.35.80.9 37109 --interface-ip LOCAL_MARKET_DATA_IP
```

`--interface-ip` is required on a multi-NIC production host so multicast joins
use the provisioned market-data interface. `0.0.0.0` remains the default for
offline/route-selected use. The JSON files under `config/` are deployment
manifests; this temporary observer currently takes the channel list on its CLI.

The output is deliberately raw. Do not infer field offsets until captures from each channel are compared. This module is disposable and is not part of the strategy or production MD plugin.

Offline validation uses loopback UDP and does not require Shanghai network access:

```text
ctest3 --test-dir build/<variant> -R sse_udp_observer_offline_test --output-on-failure
```
