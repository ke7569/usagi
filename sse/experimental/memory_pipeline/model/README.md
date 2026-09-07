# SSE v0.4 Legacy MidMix Model Skeleton

The supplied `v04-legacy-midmix-sse` bundle defines the model ABI used by the
future HStrategy factor path:

- 50 Float32 factors in `factors.txt` order; no external scaler.
- `feature_layers`: `50 -> 512 -> 256 -> 128`, Softsign after each linear.
- Residual projection `50 -> 128`, added to the nonlinear stack.
- One batch-first GRU with input 128 and hidden size 64.
- Residual `128 -> 64`, added to GRU output, then LayerNorm(64, eps=1e-5).
- Prediction head `64 -> 8 -> 1`, Softsign between the two linear layers.
- Output is permille equal-weight `15s/30s/60s` mid-to-mid return.
- Hidden state is `[1, lane, 64]`, reset at each stock-day boundary.

`legacy_midmix_sse.py` is an executable topology skeleton and target/sampling
contract. It can load `model/best.pt` from the extracted reference bundle when
PyTorch is available, but PyTorch is reference-only and is not linked into the
live process. Production inference uses `sse_model_runtime.cpp` and the fixed
`SSEMODL1.bin` artifact written by `convert_state_dict_npz.py`.

`sse_model_sequence_probe` reads a raw `N x 50` Float32 matrix, carries the
64-dimensional hidden state across the sequence, and writes `N` predictions
followed by the final hidden state. It is the non-PyTorch parity path for the
bundle's zero-input and 4,102-row continuous CPU FP32 golden cases.

The SSE factor order in `factors.txt` is a separate ABI. The existing SZE
`mix153060` implementation and legacy `SsePredictor` class are not valid
inputs to this model and are intentionally not used by the native runtime.

The SSE strategy factory is currently fail-closed for the same reason: the
offline observer and native model can be validated independently, while the
event-to-factor replay is still being built.

## SSE opening Snapshot GRU handoff

The separate 2026-08-17 Snapshot handoff is implemented in
`snapshot_gru_runtime.*` and `snapshot_ensemble.*`. It uses a 36-factor
baseline arm and a 95-factor Snapshot+Auction59 arm; see `SNAPSHOT_GRU.md` and
the two `sse_snapshot_gru_*.json` contracts. This profile is independent of
the 50-factor v0.4 runtime above and is also fail-closed until the SSE factor
adapter is connected.

`sse_hybrid_model.*` now combines the two independent event streams without
mixing their factor ABIs. Tick predictions are generated from the opening but
remain shadow outputs before 09:35; Snapshot ensemble predictions are selected
for `[09:30,09:35)`, and the already-warm tick model is selected at 09:35 and
later. See `SSE_HYBRID_MODEL.md`.
