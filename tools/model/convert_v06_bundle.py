#!/usr/bin/env python3
"""Export the accepted V06 B15/MH4 NumPy bundle without requiring PyTorch.

Native v2 keeps the v1 tensor records, adding uint32 output_count immediately
after factor_count in the little-endian header. Heads are 15/30/60/120 seconds.
The optional golden file contains the complete chronological 12,852-row day,
all FP32 stages, and both layers' final hidden state; no rows are masked out.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct

import numpy as np

from convert_mix153060_model import MAGIC, ENDIAN_MARKER, TENSOR_ORDER, pack_string, sha256

CHECKPOINT_HASH = "a56a98638e9c2357022dfd7bc4c98ce320ee8ac746540cab86783948f85bfa11"
FACTOR_HASH = "e20ed70098a025f597f8b9cda41fb79b3188d875ad227f243c872ddbfbbed97e"
HEADER = struct.Struct("<8sIIIIII f II 32s32s")
CASE = "golden/000807.SZ_20260401"
SHAPES = {
    "input_norm.weight": (50,), "input_norm.bias": (50,),
    "input_proj.weight": (128, 50), "input_proj.bias": (128,),
    "head.weight": (4, 128), "head.bias": (4,),
}
for layer in range(2):
    for part in ("ih", "hh"):
        SHAPES[f"gru.weight_{part}_l{layer}"] = (384, 128)
        SHAPES[f"gru.bias_{part}_l{layer}"] = (384,)


def verify_source(root, relative):
    checksums = {}
    for line in (root / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        digest, filename = line.split(maxsplit=1)
        checksums[filename.lstrip("*")] = digest
    if relative not in checksums or sha256(root / relative) != checksums[relative]:
        raise ValueError(f"bundle checksum mismatch: {relative}")


def checked_array(value, shape, name):
    value = np.asarray(value)
    if value.dtype != np.dtype("float32") or value.shape != shape:
        raise ValueError(f"{name}: expected float32 {shape}, got {value.dtype} {value.shape}")
    if not np.isfinite(value).all():
        raise ValueError(f"{name}: non-finite data")
    return np.ascontiguousarray(value, dtype="<f4")


def write_golden(root, output):
    relative = CASE + "/model_stage_outputs.npz"
    verify_source(root, relative)
    stages = (("raw_input", 50), ("normalized_input", 50),
              ("projected_input", 128), ("recurrent_output", 128), ("prediction", 4))
    with np.load(root / relative, allow_pickle=False) as data:
        arrays = [checked_array(data[name], (12852, width), name) for name, width in stages]
        final_hidden = checked_array(data["final_hidden"], (2, 1, 128), "final_hidden")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with temporary.open("wb") as handle:
        handle.write(struct.pack("<8sIIIII", b"V06GOLD1", 1, 12852, 50, 128, 4))
        # Each row: raw[50], norm[50], proj[128], gru[128], heads[4].
        handle.write(np.concatenate(arrays, axis=1).astype("<f4").tobytes())
        handle.write(final_hidden.tobytes())
    temporary.replace(output)


def convert(root, output, golden_output=None):
    for relative in ("MANIFEST.json", "model/best.pt", "model/state_dict.npz",
                     "model/architecture.json", "factors/factors.txt", "factors/factor_contract.json"):
        verify_source(root, relative)
    manifest = json.loads((root / "MANIFEST.json").read_text(encoding="utf-8"))
    if sha256(root / "model/best.pt") != CHECKPOINT_HASH:
        raise ValueError("checkpoint is not the accepted V06 B seed43 artifact")
    if manifest["model"]["checkpoint_sha256"] != CHECKPOINT_HASH:
        raise ValueError("manifest checkpoint mismatch")
    names = (root / "factors/factors.txt").read_text(encoding="utf-8").splitlines()
    contract = json.loads((root / "factors/factor_contract.json").read_text(encoding="utf-8"))
    if len(names) != 50 or names != contract["feature_names"]:
        raise ValueError("factor order mismatch")
    hashes = {hashlib.sha256(("\n".join(names) + suffix).encode()).hexdigest() for suffix in ("", "\n")}
    if FACTOR_HASH not in hashes or manifest["model"]["factor_names_sha256"] != FACTOR_HASH:
        raise ValueError("factor hash mismatch")
    architecture = json.loads((root / "model/architecture.json").read_text(encoding="utf-8"))
    if architecture["output_order"] != ["15s", "30s", "60s", "120s"] or architecture["layernorm_eps"] != 1e-5:
        raise ValueError("unexpected output order or normalization")
    with np.load(root / "model/state_dict.npz", allow_pickle=False) as state:
        if set(state.files) != set(TENSOR_ORDER):
            raise ValueError("unexpected state_dict tensor names")
        tensors = {name: checked_array(state[name], SHAPES[name], name) for name in TENSOR_ORDER}
    if sum(value.size for value in tensors.values()) != 205288:
        raise ValueError("parameter count mismatch")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with temporary.open("wb") as handle:
        handle.write(HEADER.pack(MAGIC, 2, ENDIAN_MARKER, 50, 128, 2, 14, 1e-5, 50, 4,
                                 bytes.fromhex(CHECKPOINT_HASH), bytes.fromhex(FACTOR_HASH)))
        for name in names:
            handle.write(pack_string(name))
        for name in TENSOR_ORDER:
            value = tensors[name]
            handle.write(pack_string(name))
            handle.write(struct.pack("<B", value.ndim))
            for dimension in value.shape:
                handle.write(struct.pack("<I", dimension))
            handle.write(struct.pack("<Q", value.nbytes))
            handle.write(value.tobytes())
    temporary.replace(output)
    metadata = {"format": "t0.mix153060.sze-v06.v2", "checkpoint_sha256": CHECKPOINT_HASH,
                "factor_names_sha256": FACTOR_HASH, "binary_sha256": sha256(output),
                "state_dict_sha256": sha256(root / "model/state_dict.npz"),
                "output_order": ["15s", "30s", "60s", "120s"], "main_prediction": "15s",
                "output_units": "permille", "external_scaler": False,
                "hidden_reset": "instrument-day boundary; retain across lunch",
                "precision": "CPU FP32; no tanh approximation"}
    if golden_output is not None:
        write_golden(root, golden_output)
        metadata["golden_rows"] = 12852
        metadata["golden_sha256"] = sha256(golden_output)
    output.with_suffix(output.suffix + ".json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--full-golden-output", type=Path)
    args = parser.parse_args()
    print(json.dumps(convert(args.bundle_root, args.output, args.full_golden_output), indent=2))


if __name__ == "__main__":
    main()
