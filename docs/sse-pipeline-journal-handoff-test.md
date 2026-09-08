# SSE Hardware-v3 Journal Handoff Check

`tests/sse/sse_pipeline_journal_handoff_test.cpp` is an isolated end-to-end
check for the two-stage Shanghai transport. It creates a fresh canonical
`sse-stream-v2` journal and SHM ring, using synthetic wire bytes and synthetic
PHC/application timestamp metadata. It does not access a NIC and its timestamp
values must not be read as physical hardware measurements.

The first datagram is written to the journal and ring, then consumed from the
journal. The consumer is advanced to `kReplayHandoff`; the second datagram is
published to SHM first and consumed from live handoff, then appended to the
journal to model the writer catching up. The hardware-v3 pipeline receives the
same decoded events and dispatches its asynchronous result only through the
owner's `poll_outputs()`.

The check asserts one closed-batch tick prediction and one matching
`kBatchEndOutput` at the exact 5,000 ns synthetic PHC boundary. It also asserts
that the second packet's newly opened batch produces no output after repeated
`finish()` calls, because shutdown must not flush an unsealed batch. A missing
hardware timestamp is rejected by the hardware-v3 processor rather than
falling back to software time.

The pipeline CPU list must contain one CPU from each requested L3 domain. The
test defaults to `112,120,128,136` and accepts `SSE_E2E_CPUS` for an isolated
host. The verification run used `64,72,80,88` because the live capture process
occupied the 112-119 L3 domain.

Observed output on the integrated tree (`57e9487`) with GCC 4.8.5:

```
sse_pipeline_journal_handoff_test: PASS replayed=1 live=1 confirmed_outputs=2 unsealed_batch_predictions=0
```
