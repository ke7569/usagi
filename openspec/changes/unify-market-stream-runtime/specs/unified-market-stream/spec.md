## ADDED Requirements

### Requirement: Owned runtime lifecycle
Supported market runtime entries SHALL expose explicit construction, start, stop, join and destruction without relying on a nonvirtual vendor stop method. The CLI and dynamic-library entry SHALL reuse the same application assembly. Stop requests issued before or during startup MUST remain effective; destruction MUST join owned workers. Legacy hosts without the required lifecycle SHALL fail explicitly rather than start an unowned receiver.

#### Scenario: Stop during startup
- **WHEN** a caller requests stop immediately before or after start
- **THEN** no new order is admitted, workers terminate and join returns without an orphaned receiver

#### Scenario: Shared-library parity
- **WHEN** the same profile and recording are executed by the CLI and versioned runtime API
- **THEN** their market/strategy summaries match without loading a broker SDK

### Requirement: Explicit account reconciliation
New-order readiness SHALL require a connected, epoch-bound and complete quiescent account snapshot matching the configured account/source/day and instrument universe. Position and open-order query completion MUST be explicit. Stale callbacks, snapshot-time activity, missing positions, unknown open orders and disconnection MUST NOT establish readiness. Paper baselines SHALL be clearly distinguished from broker reconciliation.

#### Scenario: Incomplete or stale reconciliation
- **WHEN** an account response arrives without all positions and an explicit completed empty order query, or belongs to an older epoch
- **THEN** it cannot enable new orders

#### Scenario: Disconnect after readiness
- **WHEN** a reconciled connection disconnects or a session begins stopping
- **THEN** new-order readiness closes while owned order/trade reports remain serviceable

### Requirement: Shared strategy execution boundary
The SSE stream and framework entry SHALL call the existing strategy decision implementation with the book, prediction and timestamp from the same accepted sample. Execution and time SHALL be injectable without duplicating strategy formulas. New-order eligibility SHALL be checked again at the execution boundary; recording-required failure, invalid input, processing failure, incomplete recovery and stopping MUST deny new orders. Cancellation and order/trade reports SHALL remain serviceable when new orders are blocked.

#### Scenario: Protected execution
- **WHEN** an eligible signal produces an order intent and the new-order gate then closes
- **THEN** a subsequent order cannot reach the executor, while cancellation and reports continue to process

#### Scenario: Identical offline strategy
- **WHEN** identical recorded events and explicit initial account state are supplied through capture and replay
- **THEN** the same strategy produces identical order intents using an in-memory executor without TD access

#### Scenario: Stopping
- **WHEN** a session begins stopping with pending market events and delayed cancellations
- **THEN** no new order is submitted and no destructor-triggered sample is generated

### Requirement: Authoritative per-instrument Shanghai sampling
The SSE processor SHALL judge quiet independently per instrument, strictly greater than 100us. It SHALL initialize its window at the first valid two-sided book update at or after 09:30 without emitting an initialization sample. At a quiet cut it SHALL sample when turnover increment >= HistoryAmount/8000 OR exchange-time increment >=100s OR mid-price changes by more than 1e-6 with traded volume increment >=100 shares. Each instrument MUST NOT produce two samples at the same exchange timestamp. Rejected duplicate cuts MUST NOT reset the factor/turnover window. No 09:25 seed or global quiet alternative SHALL remain in active code.

#### Scenario: Independent quiet deadlines
- **WHEN** instrument A is quiet for more than 100us while B continues receiving updates
- **THEN** A is checked at its own deadline and B cannot postpone it

#### Scenario: Repeated exchange timestamp
- **WHEN** two separate eligible bursts of A end at the same exchange timestamp
- **THEN** only the first sample is emitted and later accumulated flow remains for a future exchange timestamp

#### Scenario: Opening initialization and inclusive thresholds
- **WHEN** the first valid opening book initializes a window and a later eligible cut exactly meets the amount or 100-second threshold
- **THEN** the later cut emits a sample, without an artificial 09:25 seed or initialization sample

### Requirement: Bounded shared ingress and replay
Live and replay SHALL deliver the same ordered full-payload and idle events to a common callback, with recorded timestamp domains and channel metadata. Replay MUST reject an incomplete recording without allowing application callbacks during initial validation.

#### Scenario: Burst recording and replay
- **WHEN** a burst of 1000 complete datagrams and a snapshot are captured across segment rotation
- **THEN** replay delivers identical payloads, sequence, receive batches and timestamps, independent of replay processing speed

#### Scenario: Corrupt or incomplete input
- **WHEN** a segment is truncated, missing, corrupt or followed by unexpected files
- **THEN** replay fails before the first application callback

### Requirement: Separate recording degradation from market invalidity
The stream SHALL distinguish recording-only failure from incomplete market input. Required recording SHALL fail closed; explicitly optional recording SHALL expose a sticky fault while allowing complete market data to continue. A recorder failure MUST NOT be described as proof of invalid order-book contents.

#### Scenario: Optional disk failure
- **WHEN** writing or syncing an optional recording fails
- **THEN** market callbacks continue, recording failure is observable, and the recording is not reported clean

#### Scenario: Mandatory disk failure
- **WHEN** required recording fails
- **THEN** the stream fails with a recording-specific reason and denies its new-risk prerequisite

#### Scenario: Lost input
- **WHEN** ingress overflows or a datagram is truncated
- **THEN** processing fails regardless of recording policy and input is marked invalid

### Requirement: Market-specific processing with common drivers
Each market SHALL reuse its real decoding, book, sampler, factor and model implementation for live input and replay. Market sampling and recovery rules MUST remain explicit and independent.

#### Scenario: Receive batch differs from sample boundary
- **WHEN** a syscall batch ends without satisfying the market's sampling condition
- **THEN** no sample is emitted merely because the syscall ended

#### Scenario: Deterministic market processing
- **WHEN** the same complete recording, model, static inputs and parameters are supplied twice
- **THEN** each market's observable processing outputs match, and unavailable model/data acceptance is reported separately

### Requirement: Independent builds and safe verification
Market processing targets SHALL be independently buildable without the other market's model implementation. Verification MUST NOT log in to TD, send orders, deploy libraries or overwrite existing user changes.

#### Scenario: Offline build matrix
- **WHEN** the SSE and SZE processing targets are built and tested
- **THEN** each links its own market dependencies and tests run without a real trading connection
