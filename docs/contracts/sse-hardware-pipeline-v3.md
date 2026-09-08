# Shanghai hardware batch pipeline v3

`processing_contract=sse-hardware-batch-v3` selects hardware batching explicitly.
It accepts complete primary tick packets through the allocation-free POD decoder,
validates all six wire channels before filtering the configured universe, and
keeps capture/journal/SHM in their existing separate process. Unconfigured
instruments on other valid wire channels, including B shares on Channel 20,
participate in sequence validation and are filtered before A-share shard rules.

Each UDP subscription (`StreamEvent.channel_id`) has its own NIC PHC gap state.
A following datagram with a gap of at least 5,000 ns closes that subscription's
previous batch before the new packet is routed. The twenty tick records in a
packet are indivisible. `receive_batch`, `batch_index`, and `batch_size` only
describe the receive syscall; one syscall may contain several hardware batches,
and a hardware batch may span several syscalls. A strictly recognized 32-byte
heartbeat participates in the same timestamp/gap accounting.

Every UDP datagram, including unconfigured symbols and heartbeats, requires both
hardware timestamp flags, a nonzero hardware timestamp, and a stable PHC index.
A missing timestamp, PHC change, or backwards PHC timestamp within one UDP
subscription invalidates prediction. Different subscriptions may be delivered
in interleaved PHC order because sockets are drained separately. Snapshot packets
never rearm or close another subscription's pending tick cut.

An idle event closes each eligible subscription only after at least 5,000 ns
have passed on the recorded monotonic clock since its last datagram. This check
compares two monotonic values; PHC is never subtracted from monotonic or realtime.
`finish()` and EOF do not synthesize a market boundary: `shutdown_flush=false`.

The frontend assigns a stock on its first tick using `sse_pipeline::ShardPlan`.
Channel 1 through Channel 6 stocks are balanced across book shards, and a stock
never changes its book shard. New events that change its wire channel or tick
UDP subscription are rejected. A stock's snapshot subscription is also fixed
once observed; snapshot-only inputs do not need a tick channel assignment.

Book workers consume a bounded FIFO of decoded POD records and explicit BatchEnd
commands. Only after a BatchEnd command may a worker consume the sampling gate,
build its 50-factor row, and copy the matching ten-level book view. A snapshot
subscription's cut releases only its snapshot rows. The frontend separately
maintains previous snapshots and builds all accepted Snapshot36 rows when their
subscription's cut closes. It does not share mutable order books with inference.

Each stock has one fixed inference owner. Accepted Tick and Snapshot rows from
the same cut are ordered by input sequence and record offset; successive cuts
for that stock follow their confirmed close order. Every accepted recurrent
step is retained. Once a stock has a book route, all its cut work goes through
that book FIFO before reaching inference, including snapshot-only cuts. Before
the first tick route, snapshot work goes directly to the same inference owner.
Workers begin each ready stock's work without waiting for another stock to
produce a sample. Tick and snapshot recurrent states remain per stock.

Completed work enters a bounded queue. `poll_outputs()` runs callbacks only on
the input owner thread, in deterministic cut-close order and source order within
the cut. BatchEnd output follows the completed outputs for that cut and records
the batch ID, UDP subscription, last input sequence, last PHC timestamp, PHC
identity, clock flags, close reason, packet count, candidate count, and prediction
count. Strategy callbacks, counters and OMS calls therefore remain serialized.
Slow inference can delay output delivery, while independent book shards continue
to apply subsequent ticks.

Eligible subscriptions close in input-sequence order at an idle event. Independent
subscriptions can still finish cuts in a different exchange-time order, especially
at the 09:35 snapshot-to-tick switch. Every accepted row advances its own model
state. The v3 strategy then ignores a selected signal older than its last traded
signal time and reports `strategy.stale_signal_drops`; it does not send a stale
order or invalidate prediction. The software-v2 strategy keeps its existing
backwards-time validation.

`finish()` drains already confirmed cuts, joins every worker and releases the
L3 leases. It is idempotent and belongs to the same owner thread. Background or
callback exceptions become sticky processor errors; worker loops terminate
without blocking on a full downstream queue. Any queue or pending-cut capacity
overflow invalidates prediction rather than dropping a tick or accepted sample.
Destroying an unfinished processor stops workers without dispatching callbacks.

## Profiles and CPU placement

The profile generator defaults undeclared Shanghai configurations to the existing
software contract. Select hardware explicitly, or supply an explicit hardware
sampling declaration in the unified config:

```sh
python3 tools/config/prepare_stream_processing.py \
  --config unified-sse.json --output processing-sse.json \
  --sse-contract sse-hardware-batch-v3 \
  --sse-pipeline config/examples/sse/sse_hardware_pipeline.example.json
```

`--sse-pipeline` contains `book_cpus`, `inference_cpus`, `ingress_capacity`,
`inference_capacity`, and `output_capacity`. Each CPU list must contain 1–64
entries. `-1` chooses a free L3 domain using the existing `sse_cpu::Lease`; explicit
CPUs must also have distinct, unoccupied L3 domains. The input owner retains its
existing process CPU lease. Workers acquire their leases before starting and
bind before the processor accepts input.

Capacities are between 1 and 1,048,576. Ingress capacity counts POD commands per
book shard and also bounds outstanding raw snapshots. Inference capacity counts
stock cut work items per inference owner; one item retains all snapshot steps
accepted in that cut. Output capacity also imposes one global reservation budget
across raw pending snapshots and all retained immutable rows, in addition to
unfinished stock cut work and the completion queue. Snapshot reservations survive
the move into inference and release only on owner delivery or shutdown; accepted
tick rows reserve from the same budget. Raw snapshot buffers release capacity at
each cut. `retained_rows` and `retained_row_high_water` expose the accounting. The
default two-book example uses about 30 MB for tick FIFOs.
Power-of-two ingress capacities use a mask instead of division on the tick path.
Omitting the pipeline section runs deterministic serial v3 with the same cuts,
factors, model step order and callback order.

The hardware sampling declaration is `mode=hardware-gap-batch`, `clock=NIC_PHC`,
`threshold_ns=5000`, `comparison=greater-or-equal`, and
`activity_scope=per-udp-subscription-gap`. Old global-scope declarations must be
updated rather than labeled as subscription-local behavior.

`sse-per-instrument-v2` remains synchronous: its monotonic gap is strictly greater
than 100,000 ns, scoped to each instrument's book activity. Hardware timestamp
metadata does not switch that contract. A v2 profile cannot contain pipeline
worker settings, and a v3 profile rejects missing hardware timestamp input.

## Validation

`sse_compute_pipeline_test` exercises marker gating for tick and snapshot models,
twenty-record packets, receive syscall boundaries, strict clocks, whole-channel
sequence continuity, heartbeat handling, snapshot-only inputs, independent UDP
subscriptions, half-batch shutdown, output callback ownership, capacity errors,
worker cleanup and software-v2 compatibility. Run concurrency checks with four
available CPUs from distinct L3 domains:

```sh
SSE_PIPELINE_TEST_CPUS=80,88,96,104 build/pipeline/sse_compute_pipeline_test
```

Set `SSE_PIPELINE_REAL_PROFILE=/path/to/processing-profile.json` to run the same
test with that profile's five existing model/scaler artifacts. It reads the
artifacts without creating or modifying model files; the default test continues
to use its synthetic nonzero recurrent fixture.

The concurrency fixture compares serial and parallel output for 248 ticks and
268 accepted model samples using nonzero recurrent tick weights. It also checks
that each of the six wire channels distributes four stocks 2/2 across book
shards, and that both books apply another 100 ticks while one inference worker
processes 319 snapshot recurrences. All factor values and predictions are
compared exactly, with accepted/inferred sample counts checked independently.
The test prints input and drained durations for the fixed workload; these are
pipeline measurements for that fixture, not a live-market latency claim.
