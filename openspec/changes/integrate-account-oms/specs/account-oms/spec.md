## ADDED Requirements

### Requirement: Single account and order authority
The OMS SHALL own order identity, lifecycle, account reservations and execution-derived positions. Strategy SHALL read authoritative views without maintaining a parallel fill/pending ledger. Every send/cancel path MUST use OMS admission or remain explicitly disabled.

#### Scenario: Two markets share a funding account
- **WHEN** SZE and SSE clients submit against one OMS account
- **THEN** cash, share and self-trade checks use their combined exposure, and a second uncoordinated real owner is rejected

### Requirement: Registration before dispatch and callback safety
Identity and reservations MUST precede backend dispatch. Synchronous and other-thread callbacks MUST NOT deadlock, lose exact ownership or mutate an obsolete instance. Send-unknown MUST retain exposure until resolved.

#### Scenario: Callback arrives inside submit
- **WHEN** the backend reports an order before its send function returns
- **THEN** the registered order is found and later send-result processing cannot double-release its reservation

### Requirement: Idempotent quantity and explicit money accounting
Cumulative and deduplicated trade facts SHALL be reconciled per order using declared coverage/watermarks. Unknown overlap, execution prices and fees MUST remain explicit. Normal states MUST conserve original quantity; contradictions require reconciliation instead of silent truncation.

#### Scenario: Cumulative regression and repeat
- **WHEN** a 1000-share order receives (cum,leaves) (300,700), (0,1000), (300,700)
- **THEN** its confirmed fill remains 300 and working reservation remains 700 without duplicate release

#### Scenario: Late trade after terminal
- **WHEN** order A receives a new fill after a terminal report while B remains working
- **THEN** A's new fact is recorded without releasing B's reservation and contradictory terminal facts block new risk

### Requirement: Bounded cancellation and capability enforcement
OMS SHALL retain/coalesce cancel intent, wait for necessary IDs, apply bounded retries and recheck terminal state, identity and limits at dispatch. Native FAK and limit-then-cancel MUST be distinguishable and unsupported modes rejected.

#### Scenario: Missing ID then throttled cancellation
- **WHEN** cancellation is requested before the broker ID exists and the first eligible attempt is throttled
- **THEN** the intent remains pending, exposure is retained, and a later controlled-clock attempt obeys retry limits

### Requirement: Atomic account risk admission
OMS SHALL atomically check/reserve/register cash, sellable shares, quantity/notional/tick/lot limits, account switches, pending capacity and send/cancel rates. Own and external opposite working orders MUST participate in self-trade checks until terminal evidence.

#### Scenario: Simultaneous submissions exceed budget
- **WHEN** independent clients collectively request more than available account cash or shares
- **THEN** the accepted set remains within budget without duplicate spending or overselling

### Requirement: Complete active-order recovery
Recovery SHALL require successful scoped completion and a consistent snapshot boundary, restore owned active orders and include external exposure without granting cancellation ownership. Activity during queries, stale epochs and unresolved unknown sends MUST prevent readiness.

#### Scenario: Reconnect with active and external orders
- **WHEN** a complete snapshot identifies persisted own orders and foreign orders
- **THEN** reservations are rebuilt once, foreign exposure limits new risk, and only proven owned orders can be canceled

### Requirement: Durable intent and observable failure
Non-simulated dispatch MUST have a synchronized intent record first. Recovery MUST validate sequence/length/checksum, preserve corrupt tails and never replay sends. Journal, queue, timer or identity-capacity failure MUST stop new risk while defining cancellation/report handling.

#### Scenario: Crash between send and recorded result
- **WHEN** a durable intent exists without a conclusive send result after restart
- **THEN** the order remains uncertain, is not resent and requires reconciliation before new risk

### Requirement: Actual market runtime integration
Both market entry points SHALL use the same OMS implementation with explicit account/backend configuration. Paper SHALL remain non-filling; scripted fault reports SHALL use the same production ledger and controlled clock. Tests MUST execute real integration and always-active invariants.

#### Scenario: Offline strategy to OMS and back
- **WHEN** each market processes fixed input with the scripted backend
- **THEN** strategy intent passes OMS admission, backend reports update OMS state, and the next strategy decision reads that state without a legacy bypass

### Requirement: Explainable latency measurements
OMS diagnostics SHALL distinguish pre-backend dispatch, backend call and post-backend bookkeeping latency using identical preconstructed intents. Detailed probes MUST be absent from normal runtime targets, leave trading/durability behavior unchanged, and disclose their overhead and workload boundaries.

#### Scenario: Compare instrumented and normal submission
- **WHEN** the same bounded Paper workload runs with normal and diagnostic core builds
- **THEN** orders, reservations, timers and outbound commands agree, and measured internal stages are reported separately from uninstrumented end-to-end cost

### Requirement: Compact asynchronous audit writes
Ordinary hot-path journal events SHALL use bounded compact records without JSON formatting or disk waits on the submitting thread. A bounded background writer SHALL preserve record order and expose failures and accepted/written/durable watermarks. Explicit pre-send durable intents MUST retain their existing persistence guarantee. Legacy journals MUST remain readable.

#### Scenario: Background writer is slow or fails
- **WHEN** journal capacity is exhausted or the worker cannot write accepted records
- **THEN** the producer does not wait for ordinary log output, new risk is disabled, and no asynchronous enqueue is misreported as durable completion
