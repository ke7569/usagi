# SSE Wait and Batch-End Contract

`MarketDataStream::run` keeps its existing call sites valid and accepts an
optional owner-thread poll callback. When the ingress queue is empty, the
dispatcher invokes that callback before yielding. The callback is not an input
event and cannot create a journal record. A callback exception marks processing
invalid through the existing stream failure path.

The Shanghai journal prediction loop calls `poll_outputs()` on every empty SHM
read. It uses `pause` for 4,096 iterations and then one scheduler yield, which
keeps the prediction owner responsive without a fixed 100 us sleep. At exit it
calls `begin_stop()`, then `finish()`, and only then builds the final summary.
The pipeline `finish()` contract drains already-closed BatchEnds; it does not
close or flush an open final batch. Serial applications without these optional
methods remain no-ops.

## Hardware idle configuration

The existing `schema_version=1` journal configuration remains strict:
`idle_gap_ns` must be 100,000 ns. The new
`journal_capture_hardware_v2.example.json` uses `schema_version=2`,
`payload_format=sse-stream-v2`, a nonempty hardware interface, and
`idle_gap_ns=5000`. Schema v2 permits a positive configured idle gap up to one
second, but it is intentionally limited to the hardware timestamp transport.

The receiver records an idle observation from `CLOCK_MONOTONIC` after the last
successful `recvmmsg()` return. When the timer expires it performs a zero-time
ready-socket poll and drains any packets visible there before publishing idle.
The timer is rearmed only after a later successful receive batch. This ordering
prevents a stale idle marker from preceding packets already delivered to the
socket layer, while retaining the old software-stream strict-greater idle
contract.

The 5 us sampling rule is a separate PHC rule: adjacent packets on the same
PHC are in different batches when their hardware timestamp gap is at least
5,000 ns. A kernel socket timeout or this MONOTONIC idle observation cannot
prove that the physical NIC link was silent for 5 us. Hardware-v3 processing
must therefore require the packet hardware timestamp, PHC identity, and the
per-subscription elapsed-gap contract; missing hardware data is rejected rather
than silently replaced by software time. Short idle settings require actual
NIC-load acceptance testing.

## Isolated checks

With GCC 4.8.5, the focused transport test passed for schema v1/v2 and clock
flags. The CPU112 loopback test passed with `idle_gap_ns=5000`, verified five
datagrams with `receive_batch_size=4` before one idle, a second datagram/idle
pair, no duplicate idle, owner polling, and stop response. It does not claim
physical PHC silence; only a hardware NIC probe and the downstream PHC contract
can establish that property.
