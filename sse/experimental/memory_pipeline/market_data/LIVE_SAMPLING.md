# SSE live batch-end sampling

The production integration point is `sse_batch_end_sampler.*`. It implements
the live form of the frozen `v0.4-sse-cob-batch-end-100us` contract without a
periodic 10kHz MD publisher.

## Event-loop contract

For every normalized SSE update on one ordered logical feed lane:

```cpp
BatchEnd closed;
bool did_close = false;
sampler.advance_to_event(event.monotonic_receive_ns,
                         &closed, &did_close, &error);
if (did_close) {
    // The new event has not touched the book yet.
    emit_batch(closed);
}

apply_event_to_sse_book(event);

Candidate candidate;
Candidate* candidate_ptr = 0;
if (event.is_complete_orderbook_sh_l2) {
    candidate = metadata_from(event);
    candidate_ptr = &candidate;
}
sampler.commit_applied_event(candidate_ptr, event.sequence_healthy, &error);
timer.arm_absolute(sampler.next_deadline_ns(), &error);
```

When the timer fd becomes readable, consume it, read `CLOCK_MONOTONIC`, and
call `on_timer`. If a batch closes, process it on the same serialized book
owner before accepting another update. Empty batches do not generate factors
or advance model state.

The timer deadline is the first nanosecond that satisfies the strict rule:
`last_activity + 100000 + 1`. An event at exactly 100 microseconds remains in
the same batch. A stale timer is harmless because `on_timer` rechecks the
current last-activity timestamp.

## Downsample and prediction order

For each candidate emitted at batch end:

1. Materialize the CompleteOrderBookSH cut from the reconstructed SSE book.
2. Call the instrument's `TickSampleGate` with its positive daily turnover
   threshold.
3. If accepted, construct the frozen 50 factors in `model/factors.txt` order.
4. Call `sse_hybrid_model::Model::on_tick` with the instrument/day state.
5. Log candidate receive time, batch emission time, exchange time, factor row,
   selected model source, prediction, and accepted-row count.

Batch timing uses local monotonic receive time. Continuous-session checks and
the 09:35 model handover use the original SSE exchange time.

## Feed health

On a channel sequence gap, decoder reset, or non-monotonic local timestamp,
call `mark_sequence_gap`. Discard the pending batch and do not call the model.
Only call `recover` after the decoder/book reports a completed resync. Use
`reset_trading_day` before the first event of each new trading day.

Detector instances are keyed by logical sequenced feed lane. The first live
capture must establish whether the vendor channels have independent batch
boundaries or share a cross-channel watermark.
