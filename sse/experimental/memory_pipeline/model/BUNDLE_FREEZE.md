# Reference Bundle Freeze

Source: `ref/v04-legacy-midmix-sse.tar.zst`, bundle id `v04-legacy-midmix-sse`.

Authoritative files and SHA-256 values:

- `factors/factor_contract.json`: `ba215df81ae9c9f43f98cc32de33b6052f1268f096c09c42c88f8efefcf1be3e`
- `model/architecture.json`: `8fb08829c965c013ca8f2a419f7ba55763db12ea2b23efa01a075b8e2868ad42`
- `model/best.pt`: `b4e865694476c21cf913777bdb16b92dc4b74f4f10507a9d0c1e90436916429a`
- `model/state_dict.npz`: `10fcb81868a84db9636c65ca86effb72ab829a6ef111ebbd97a88dd6f1f1f7f0`

Model facts:

- exchange: SSE only
- 50 Float32 inputs, exact `factors.txt` order, no external scaler
- `ivo_residual_feature_stack`, 243025 parameters
- one GRU layer, hidden 64, state shape `[1, lane, 64]`
- target: equal-weight 15s/30s/60s mid-to-mid return in permille
- candidate samples: CompleteOrderBookSH Level2 batch ends with strict local gap `>100us`

The bundle remains a reference artifact under `/home/t0/ref`; this repository
stores only the ABI and topology skeleton, not the production checkpoint.
