# Common Market Data Stream

Status: transport/capture/replay implemented and locally tested. Market processing integration is tracked in `openspec/changes/unify-market-stream-runtime/tasks.md`. No production plugins have been replaced, and no real TD has been used.

## Thread and Timing Contract

```text
UDP channels -> receive thread -> bounded ingress queue -> application callback
                                                       -> bounded recording queue -> writer
recorded directory -----------------------------------> same application callback
```

`MarketDataStream` performs nonblocking `recvmmsg` with up to 64 messages per ready channel per poll iteration. It does not wait for a full batch. Both queues allocate and touch their payload storage before receive starts. Default payload storage is 32 MiB per queue (4096 slots x 8192 bytes); recording adds the second queue and a 1 MiB writer buffer. This absorbs a finite burst, not an indefinitely slower consumer. Capacity must be sized for peak input rate times worst expected consumer pause, with headroom. Oversized datagrams fail instead of being truncated.

Each queue is SPSC. Channel identity is numeric in the native callback. Callbacks are serialized; borrowed payload memory is valid only until the callback returns. Receive performs no JSON formatting, file writes, CRC calculation or disk sync. The writer owns CRCs, batched writes, file rotation and periodic `fdatasync`. The dispatcher enqueues recording before invoking the application, without waiting for durable storage.

`monotonic_ns` is sampled immediately after each receive syscall; messages from that syscall share it. It is not a per-packet NIC timestamp. `realtime_ns` is the kernel socket timestamp when available and is explicitly flagged; it is never compared directly to a monotonic deadline. `receive_batch` identifies a syscall, not an exchange batch or sample boundary.

After receive inactivity strictly greater than `idle_gap_ns`, the receiver emits an explicit global transport idle event. It is a recorded clock observation, not an SSE sampling decision. The processor advances all due instrument deadlines before mutating the next event's book, and feeds recorded idle observations through the same `on_timer` path during replay. Another stock or a Snapshot can advance observed time without rearming stock A's deadline. This is not a complete scheduler for order cancellation or TD callbacks. Cross-channel ordering is the serialized receiver's observed order, not a claim about global exchange order. Callbacks run when the recorded time observation reaches the processor; the SSE 100 us threshold is not a hard wall-clock response guarantee.

## Recording Policy and Health

`recording_required=true` is the default and requires a nonempty directory. To explicitly disable recording in a library consumer, use an empty directory and `recording_required=false`; the legacy UDP wrapper does this. A required-recording failure returns failure and denies the stream's new-risk prerequisite. Setting it explicitly false with a directory permits continued complete market processing after a recording-only failure; recording is disabled for the rest of that run, its error remains sticky, and the directory is not declared clean. Both modes retain error visibility through `health()` and `recording_error()`; the CLI reports it on stderr and in JSON.

Ingress overflow, UDP truncation and detected socket drops always fail and mark input invalid. Processing exceptions invalidate processing separately. These are not equivalent to a recording-only error. `health().permits_new_risk()` is only a transport prerequisite: it does not assert readiness, a recovered book, account permission, a current session, or a functioning TD. The actual strategy gate must combine those conditions. This library does not cancel orders, close TD, flatten positions or authorize trades.

The health/error methods can be inspected while processing; `stats()` is read only after `run/replay` returns. `SO_RXQ_OVFL` reports socket drops on subsequently delivered packets; absence of that signal is not proof of complete exchange input. Market sequence validation remains essential.

`ready()` reports receive socket/buffer initialization; it is not book or trading readiness. The CLI announces it on stderr. Tests wait for actual readiness rather than assuming startup completes in a fixed number of milliseconds.

## Durability and Stop

Segments default to 256 MiB; sync interval defaults to 100 ms. `written_sequence` advances after successful writes, and `durable_sequence` only after successful `fdatasync`. Queue backlog, OS scheduling and disk stalls mean 100 ms is not a guaranteed maximum crash-loss window. Async capture does not make each trading decision durable before it is acted on.

Normal `stop()` ends receive and drains accepted ingress and recording events, then writes a clean END marker and syncs. Application code must disable new trading decisions before draining on shutdown. There is no synthetic EOF sample or destructor flush into a strategy. The CLI handles SIGINT/SIGTERM; SIGKILL or process/power failure can leave an incomplete recording. Stop cannot guarantee a deadline while disk I/O is stalled.

Corrupt/incomplete recordings fail closed; forensic salvage and checkpoint-to-live handoff are separate future capabilities. Existing SZE `.szej` recovery/journal/SHM contracts are not replaced by this format.

## Format v1

The format is little-endian and ABI-size checked. A recording directory must be new; existing directories are never overwritten. It contains only contiguous `stream_000000.t0md`, `stream_000001.t0md`, etc.

| Element | Size | Contents |
| --- | --- | --- |
| File header | 64 bytes | Magic/version/endian, segment/stream identity, first sequence, payload bound, channel count, idle gap, CRC |
| Channel table row | 104 bytes | Name, group, interface IPv4, port |
| Event header | 72 bytes | Kind/CRC/length, sequence, monotonic/realtime time, receive batch/index/size, channel and source |
| Payload | Recorded length | Complete UDP payload, without protocol-dependent truncation |

Datagram and idle events are delivered to consumers. END is a file integrity marker only. Replay preflights the entire directory, then reads again to deliver callbacks while checking integrity again. Input must remain immutable throughout both passes; CRC is not a security signature. Replay does not sleep, synthesize time, or advance the writer's durability watermark (`durable_sequence=0`).

## Commands and Checks

From an independently configured build directory:

```bash
cmake3 --build . --target t0_md_stream market_data_stream_test sse_udp_observer -j4
ctest3 --output-on-failure -R '^(market_data_stream_test|market_data_stream_cli_test|sse_live_sampling_test|sse_udp_observer_offline_test)$'
./t0_md_stream capture /home/usagi/config/examples/stream_capture.example.json
./t0_md_stream replay capture-example-new
```

The example uses loopback, does not send packets or connect TD, and must use a different recording directory on every capture. Production feed addresses, CPU affinity and receive-buffer limits need explicit environment configuration. The old `sse_udp_observer` remains a prefix-only diagnostic; it is not a full recording substitute. Its legacy UDP wrapper now delivers serialized callbacks.

Unified configuration supports `--record-format t0md-v1 --recording /path/to/directory --time-basis recorded-receive`. Run manifests hash the complete contiguous segment set; exchange-time substitution is rejected. Hash verification does not prove model/runtime parity.

Local coverage includes a 1000-datagram burst plus full snapshot, segment rotation, event metadata and sampler replay comparison, strict 100 us boundaries, both recording policies, ingress overflow, file-limit write failure, truncated/corrupt/non-contiguous files and SIGTERM. The final per-instrument regression also covers the 10000-instrument sampler stress case, quiet stock A while B remains active, same-exchange-time accumulated flow, and first valid opening-book initialization. All 14 related CTest entries pass; detailed counts and scope are in the execution record. Synthetic queue-lag output includes an intentionally slow callback; it is not a production latency benchmark. Model, complete-day, actual NIC and broker validation are reported separately.

## Native Market Processing

`sze_stream_processing` and `sse_stream_processing` reuse the actual market-specific decoders, books, factors and model classes. Their CLI frontends are separately linked as `t0_sze_stream` and `t0_sse_stream`, both built from the same driver source. Neither binary links Deepwin strategy/TD plugins or the other market's model. CMake switches `T0_BUILD_SZE_STREAM_PROCESSOR` and `T0_BUILD_SSE_STREAM_PROCESSOR` independently select these targets.

The SZE contract is `sze-mix153060-v04`. It uses the latest reviewed opening boundary, wire feed sequence tracking, the existing mix153060 runtime, and recorded realtime for the existing normalization contract. Idle/EOF never flush pending state. Sequence gaps and book errors remain sticky; it does not reconnect, infer missing events, import SHM state or perform journal-to-live recovery. Existing SZE recovery remains a separate compatibility path pending integration.

The SSE contract is `sse-per-instrument-v2`, implementing the user-confirmed rules of 2026-09-06. Quiet must be strictly greater than 100000 ns for the individual stock. The indexed sampler holds at most one deadline per configured instrument and updates that entry in place. `BatchEndSampler` is constructed with the configured IDs; `advance_to_event` and `on_timer` return `vector<BatchEnd>`, and `commit_applied_event(id, candidate, sequence_healthy)` updates only that stock. There is no active `MonotonicOneShotTimer` implementation alongside the recorded-clock path.

The first book-changing tick at or after exchange time 09:30:00 that creates valid bid and ask prices and quantities initializes the stock's window immediately, with no sample. Initialization does not wait for a quiet cut and does not use a 09:25 seed. Subsequent quiet cuts accept when turnover increase is `>= HistoryAmount/8000` in currency units, exchange-time increase is `>= 100000000` microseconds (100 seconds), OR absolute mid-price change is `> 1e-6` currency units AND traded volume increase is `>= 100` shares. At most one sample is accepted per stock at a given exchange timestamp; same-time and other rejected cuts do not clear flow or reset the window. The tick volume gate uses shares while factor state retains wire volume units.

The processor consumes complete tick and snapshot records, emits tick factors and same-sample reconstructed depth, and keeps Snapshot36 inputs independent. Snapshot-ineligible one-sided quotes are skipped, not treated as corrupted transport. Prediction requires real weights and an explicit finite Auction59 input for every configured instrument; there is no silent NaN/zero fallback. The earlier global timer and archived `PredictionEngine` rules are historical and superseded, not unresolved alternatives. Full-day comparison and old-plugin migration remain separate work; see `openspec/changes/unify-market-stream-runtime/evidence/source-provenance.md` and [SSE sampling contract](sse-sampling.md).

```bash
python3 -B tools/config/prepare_stream_processing.py \
  --config /path/to/unified-live.json --output /path/to/processing-live.json --factors-only
./t0_sze_stream capture /path/to/stream-network.json /path/to/processing-live.json

python3 -B tools/config/prepare_stream_processing.py \
  --config /path/to/unified-replay.json --output /path/to/processing-replay.json --factors-only
./t0_sze_stream replay /path/to/recording-directory /path/to/processing-replay.json
```

Use `t0_sse_stream` with SH profiles. SSE requires 100000 ns transport idle metadata in capture and replay, checked before processor callbacks; individual stocks still own their sampling deadlines. Profiles carry `sse-per-instrument-v2` and reject the old `sse-batch-end-v1` label. Sampling declarations use `activity_scope=per-instrument-sse-book-update`, `same_exchange_time_policy=at-most-one-sample`, and `initial_window=first-valid-book-at-or-after-open`; conflicting values are rejected. Profiles are generated from the validated unified config, preserve its fingerprint, and always require `execution=disabled`. Network/queue settings remain a separate transport profile. Both modes share the same processor; `--factors-only` is an explicit no-model validation mode and does not relax the input configuration schema. Without that flag, models must load before receive starts. CLI summaries contain sample/prediction counts and a diagnostic factor CRC, not a substitute for full output comparison.

Artifact/run locks must still be explicitly verified with `unified_config.py`; these CLIs do not automatically verify model SHA-256 locks or the profile fingerprint against its original unified JSON. The fingerprint is a provenance label, not a runtime verification claim. Complete application parity (strategy decisions, risk budget, TD, scheduler and recovery) is not asserted by these standalone tools.
