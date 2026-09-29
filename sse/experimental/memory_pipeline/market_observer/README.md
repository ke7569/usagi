# SSE UDP Market Observer

This temporary tool observes the Shanghai feed without decoding or forwarding it to Deepwin. It uses the reusable channel runtime in `sse/experimental/memory_pipeline/udp_runtime/UdpChannelRuntime.cpp`; each configured channel owns one UDP multicast socket and records one JSONL row per datagram:

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

## Shanghai CPU placement

SSE receive workers use one CPU per distinct L3 cache domain. The observer
and source-89 `MDEngineSSE` discover the allowed CPUs and Linux level-3 cache
topology at startup, exclude domains occupied by existing SSE processes, and
hold per-domain leases until shutdown. Missing topology, insufficient domains,
conflicting explicit assignments, or failed affinity binding cause startup to
fail. The planner does not change system-wide CPU or IRQ affinity.

The default is automatic placement. For an explicit assignment, pass one CPU
per channel with `--cpu-list 0,8` (only if those CPUs are in distinct available
domains on the target machine). `--cpu N` remains valid for a single channel.
The capture launcher forwards `--cpu-list` or `SSE_CAPTURE_CPU_LIST`.
Each source-89 JSON channel can instead specify `"cpu": N`; omitted CPUs are
selected automatically. Startup logs report each channel's CPU and L3 domain.
The observer's waiting coordinator shares the first receive worker's CPU.

Leases are `/tmp/usagi-sse-l3-UID-DOMAIN.lock` files with process-lifetime locks;
an empty file left after exit does not reserve a domain. All cooperating SSE
capture processes should run under the same service user. Legacy processes are
detected through `/proc`, as in the previous SSE planner; an unpinned legacy
thread can still migrate after discovery. For guaranteed separation, bind the
other Shanghai workloads too. Shenzhen reception, CPU settings, journal layout,
and retention policy are unaffected.

Offline validation uses loopback UDP and does not require Shanghai network access:

```text
ctest3 --test-dir build/build/<run_name> -R sse_udp_observer_offline_test --output-on-failure
```
