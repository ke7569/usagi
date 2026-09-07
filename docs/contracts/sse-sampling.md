# SSE Per-Instrument Sampling

The current stream contract is `sse-per-instrument-v2`, implementing the
user-confirmed sampling rules of 2026-09-06 in `sse_batch_end_sampler.*` and
`sse_stream_processor.*`. Each configured stock owns its quiet deadline and
sampling window. The model's `v0.4-sse-cob-batch-end-100us` factor ABI label
remains unchanged; it does not select a legacy sampling implementation.

## Serialized Event Processing

Create `BatchEndSampler` with the configured instrument IDs. Both
`advance_to_event` and `on_timer` return a vector because one time observation
can close several instruments' pending cuts. Process all returned cuts before
the incoming event changes any book:

```cpp
BatchEndSampler sampler(configured_instruments);
std::vector<BatchEnd> closed;

if (!sampler.advance_to_event(event.monotonic_receive_ns, &closed, &error))
    fail(error);
for (const BatchEnd& batch : closed) process_closed_batch(batch);

apply_event_to_sse_book(event);
initialize_window_if_first_valid_open_book(event);

const Candidate* candidate = candidate_from_book_update_or_null(event);
if (!sampler.commit_applied_event(event.instrument_id, candidate,
                                  event.sequence_healthy, &error))
    fail(error);
```

The processor calls `commit_applied_event` only for the configured stock whose
tick event was applied. A null candidate can rearm that stock's existing
pending cut, but cannot arm a different stock. Another stock's update or any
Snapshot message can advance observed time and close a due cut; it cannot
extend that cut's quiet deadline. Sequence checks still cover the whole wire
channel, including securities outside the configured stock universe.

For a recorded idle observation, call
`on_timer(observation.monotonic_ns, &closed, &error)` and process the returned
cuts on the same serialized owner. There is no separate production
`MonotonicOneShotTimer` or timer-fd path. `MarketDataStream` records global
transport idle observations, not market sampling decisions; live and replay
both pass these observations to this same processor.

Quiet must be strictly greater than 100000 ns for the individual stock. The
earliest representable deadline is `last_activity + 100000 + 1`; exactly
100 us does not close the cut. The callback runs when a datagram or idle
observation with a due recorded time reaches the processor. This is not a
guarantee that the callback runs within 100 us of wall-clock packet arrival.
Receive batching, queue backlog and scheduling can delay that observation.

The sampler uses an indexed min-heap with at most one deadline per configured
instrument. An update changes its heap entry in place; it does not append
stale timers indefinitely. `next_deadline_ns()` exposes the earliest pending
deadline, and `pending_instruments()` is bounded by the configured universe.

## Window and Gate

Immediately after the first book-changing tick at or after exchange time
09:30:00 produces valid positive bid and ask prices and quantities, initialize
that stock's window. Store its mid price, cumulative turnover, cumulative
volume and exchange time, seed the factor window and discard earlier flow.
This initialization emits no sample and advances no model state. It happens
on the book update, without waiting for the first quiet cut; no 09:25 seed is
used.

At an eligible quiet cut, take differences from the initialization cut or the
last accepted sample. Accept when any one of these conditions is true:

1. Turnover increase is `>= HistoryAmount / 8000`, in currency units, matching
   the normalized book turnover and daily `HistoryAmount`.
2. Exchange-time increase is `>= 100000000` microseconds, or 100 seconds.
3. Absolute mid-price change is `> 1e-6` currency units and traded volume
   increase is `>= 100` shares.

The volume gate converts the book's wire quantity units to shares; factor
state continues to use its existing wire quantity units. The three conditions
are OR alternatives, with AND only inside the third condition.

A stock emits at most one accepted sample for the same exchange timestamp.
A later cut with that same timestamp does not emit, clear accumulated flow,
or reset the sampling window. Rejected quiet cuts also leave the window in
place. After an accepted cut, generate the 50 factors and matching book view,
advance the instrument's model state, and make that cut the next window
start. Local monotonic time controls quiet eligibility; exchange time controls
the 100-second gate and the existing 09:35 model-source switch.

Profile declarations are `activity_scope=per-instrument-sse-book-update`,
`same_exchange_time_policy=at-most-one-sample`, and
`initial_window=first-valid-book-at-or-after-open`. Conflicting declarations
and the old `sse-batch-end-v1` stream profile are rejected.

## Health and Integration

A channel sequence gap or non-monotonic local timestamp invalidates the
processor and discards pending candidates. Sampler `recover()` is a low-level
operation, not a completed book resync: the standalone processor does not
automatically recover a damaged book or implement replay-to-live handoff.
Start each trading day with fresh processor/book/model state. EOF and shutdown
do not synthesize a final model sample.

The archived `PredictionEngine::sample_tick` and prior global quiet algorithm
are historical evidence only; their differing triggers were superseded by
the confirmed rules above. The immutable review archive remains available.
Existing Deepwin strategy plugins have not been migrated by this standalone
processor change. Updated regression results are recorded in
`docs/market_stream_execution_20260906.md`.
