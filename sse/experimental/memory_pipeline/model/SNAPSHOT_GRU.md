# SSE opening Snapshot GRU

This is a separate model profile from `v04-legacy-midmix-sse`.  The supplied
2026-08-17 handoff contains two `legacy-residual-gru` arms:

- `baseline`: 36 ordered `snapshot36` factors;
- `auction59`: the same 36 factors followed by 59 ordered
  `auction59-v1.1.0` factors.

Each arm is `feature -> 512 -> 256 -> 128` with Softsign activations, a 64
unit GRU, a 64 unit residual projection, LayerNorm and `64 -> 8 -> 1` with a
Softsign hidden head.  Hidden state is independent per instrument and trading
day.  The two arms must both advance on every accepted Snapshot row; ensemble
weights are applied only to the two final scalar predictions.

## Offline conversion

The converter uses only Python's standard library and reads the handoff's
authoritative `best.pt` (or a deliberately selected `model.pt`) zip directly.
It does not import PyTorch:

```bash
python3 sse/experimental/memory_pipeline/model/convert_snapshot_gru.py \
  --checkpoint <handoff>/models/baseline/best.pt \
  --scaler <handoff>/models/baseline/baseline.json \
  --feature-count 36 \
  --output baseline.ssegru

python3 sse/experimental/memory_pipeline/model/convert_snapshot_gru.py \
  --checkpoint <handoff>/models/auction59/best.pt \
  --scaler <handoff>/models/auction59/auction59.json \
  --feature-count 95 \
  --output auction59.ssegru
```

The output is the endian-stable `SSESGRU1` artifact consumed by
`snapshot_gru_runtime.*`.  The serving process links no Python or PyTorch.

## Serving contract

`snapshot_ensemble.*` accepts ready-to-run 36 and 95 dimensional vectors and
uses `sse/experimental/memory_pipeline/config/sse_snapshot_gru_routing.json`:

| SSE event time | baseline | Auction59 |
|---|---:|---:|
| `[09:30,09:31)` | 0.25 | 0.75 |
| `[09:31,09:34)` | 0.50 | 0.50 |
| `[09:34,09:35)` | 0.75 | 0.25 |

The route clock is exchange event time in microseconds, not local receive
time.  Outside the half-open window, for another exchange, or when either
arm is invalid/non-finite, output is rejected.  There is no silent single-arm
fallback.

Factor construction and Auction59 sidecar generation are intentionally not
hidden inside this model library.  They remain separate SSE market-data and
factor modules so the model can be replayed offline before live connectivity.

The handoff is marked `candidate_requires_independent_validation` and
`production_approval=false`; this implementation does not change that status.
