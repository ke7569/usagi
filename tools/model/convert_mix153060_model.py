#!/usr/bin/env python3
"""Convert the mix153060 handoff checkpoint to the native pipeline format.

The input is intentionally the extracted handoff directory rather than a
PyTorch object.  This keeps conversion deterministic and makes the tensor
contract auditable in environments where PyTorch is not installed.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

import numpy as np


MAGIC = b"MIX15306"
VERSION = 1
ENDIAN_MARKER = 0x01020304
HEADER = struct.Struct("<8sIIIIII f I 32s32s")
TENSOR_ORDER = (
    "input_norm.weight",
    "input_norm.bias",
    "input_proj.weight",
    "input_proj.bias",
    "gru.weight_ih_l0",
    "gru.weight_hh_l0",
    "gru.bias_ih_l0",
    "gru.bias_hh_l0",
    "gru.weight_ih_l1",
    "gru.weight_hh_l1",
    "gru.bias_ih_l1",
    "gru.bias_hh_l1",
    "head.weight",
    "head.bias",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def factor_names(root: Path) -> List[str]:
    contract = json.loads((root / "factors/factor_contract.json").read_text())
    names = list(contract.get("factor_order", contract.get("factor_names", [])))
    if not names:
        raise ValueError("factor contract has neither factor_order nor factor_names")
    listed = [line.strip() for line in (root / "factors/factors.txt").read_text().splitlines() if line.strip()]
    if names != listed:
        raise ValueError("factor_contract.json and factors.txt have different order")
    if len(names) != 50:
        raise ValueError("expected exactly 50 factors")
    expected_hash = contract.get("feature_schema_metadata", {}).get("factor_names_sha256")
    actual_hashes = (
        hashlib.sha256("\n".join(names).encode("utf-8")).hexdigest(),
        hashlib.sha256(("\n".join(names) + "\n").encode("utf-8")).hexdigest(),
    )
    if expected_hash and expected_hash not in actual_hashes:
        raise ValueError(
            "factor name hash does not match the documented contract: "
            f"{expected_hash} not in {actual_hashes}"
        )
    return names


def checked_tensors(root: Path) -> Dict[str, np.ndarray]:
    path = root / "model/state_dict.npz"
    state = np.load(path, allow_pickle=False)
    missing = [name for name in TENSOR_ORDER if name not in state.files]
    extra = [name for name in state.files if name not in TENSOR_ORDER]
    if missing or extra:
        raise ValueError(f"tensor contract mismatch; missing={missing}, extra={extra}")
    tensors = {}
    for name in TENSOR_ORDER:
        value = np.asarray(state[name])
        if value.dtype != np.dtype("float32"):
            raise ValueError(f"{name}: expected float32, got {value.dtype}")
        if not value.flags.c_contiguous:
            value = np.ascontiguousarray(value)
        tensors[name] = value
    expected_shapes = {
        "input_norm.weight": (50,),
        "input_norm.bias": (50,),
        "input_proj.weight": (128, 50),
        "input_proj.bias": (128,),
        "gru.weight_ih_l0": (384, 128),
        "gru.weight_hh_l0": (384, 128),
        "gru.bias_ih_l0": (384,),
        "gru.bias_hh_l0": (384,),
        "gru.weight_ih_l1": (384, 128),
        "gru.weight_hh_l1": (384, 128),
        "gru.bias_ih_l1": (384,),
        "gru.bias_hh_l1": (384,),
        "head.weight": (1, 128),
        "head.bias": (1,),
    }
    for name, shape in expected_shapes.items():
        if tuple(tensors[name].shape) != shape:
            raise ValueError(f"{name}: expected shape {shape}, got {tensors[name].shape}")
    return tensors


def pack_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) > 0xFFFF:
        raise ValueError("metadata string is too long")
    return struct.pack("<H", len(encoded)) + encoded


def write_golden_fixture(
    path: Path, groups: Iterable[Tuple[int, np.ndarray, np.ndarray]]
) -> None:
    groups = list(groups)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("wb") as handle:
        handle.write(struct.pack("<8sIIII", b"MIXGOLD2", 2, len(groups), 0, 50))
        for stock_order, raw_rows, predictions in groups:
            raw_rows = np.asarray(raw_rows, dtype="<f4")
            predictions = np.asarray(predictions, dtype="<f4")
            if raw_rows.ndim != 2 or raw_rows.shape[1] != 50:
                raise ValueError(f"stock {stock_order}: invalid golden feature shape {raw_rows.shape}")
            if predictions.shape != (raw_rows.shape[0],):
                raise ValueError(f"stock {stock_order}: golden prediction count mismatch")
            handle.write(struct.pack("<II", stock_order, raw_rows.shape[0]))
            for row, prediction in zip(raw_rows, predictions):
                handle.write(row.tobytes(order="C"))
                handle.write(prediction.tobytes())
        handle.flush()
    temporary.replace(path)


def write_full_golden_fixture(root: Path, path: Path, names: List[str]) -> None:
    try:
        import pyarrow as pa
        import pyarrow.ipc as ipc
    except ImportError as exc:
        raise RuntimeError("--full-golden-output requires pyarrow") from exc

    def read_arrow(arrow_path: Path):
        with pa.memory_map(str(arrow_path), "r") as source:
            return ipc.open_file(source).read_all()

    continuous_features = root / "golden/continuous_features.arrow"
    continuous_predictions = root / "golden/continuous_predictions.arrow"
    if continuous_features.exists() and continuous_predictions.exists():
        features = read_arrow(continuous_features)
        predictions = read_arrow(continuous_predictions)
        values = np.column_stack([np.asarray(features[name]) for name in names])
        expected = np.asarray(predictions["pred_cpu_fp32"])
        groups = [(0, values, expected)]
        path.parent.mkdir(parents=True, exist_ok=True)
        write_golden_fixture(path, groups)
        return

    features = read_arrow(root / "golden/features.arrow")
    predictions = read_arrow(root / "golden/predictions.arrow")
    feature_selection = np.asarray(features["selection_order"])
    prediction_selection = np.asarray(predictions["selection_order"])
    groups = []
    for stock_order in range(5):
        feature_mask = feature_selection == stock_order
        prediction_mask = prediction_selection == stock_order
        values = np.column_stack([np.asarray(features[name])[feature_mask] for name in names])
        expected = np.asarray(predictions["pred_cpu_fp32"])[prediction_mask]
        groups.append((stock_order, values, expected))
    path.parent.mkdir(parents=True, exist_ok=True)
    write_golden_fixture(path, groups)


def write_model(root: Path, output: Path, names: Iterable[str]) -> dict:
    names = list(names)
    tensors = checked_tensors(root)
    best = root / "model/best.pt"
    summary = json.loads((root / "model/checkpoint_summary.json").read_text())
    checkpoint_hash = sha256(best)
    documented_hash = summary.get("checkpoint_sha256")
    if documented_hash and checkpoint_hash != documented_hash:
        raise ValueError("best.pt checksum differs from checkpoint_summary.json")
    factor_contract = json.loads((root / "factors/factor_contract.json").read_text())
    factor_hash_text = factor_contract.get("factor_names_sha256")
    if not factor_hash_text:
        factor_hash_text = factor_contract.get("feature_schema_metadata", {}).get(
            "factor_names_sha256"
        )
    if not factor_hash_text:
        raise ValueError("factor contract is missing factor_names_sha256")
    factor_hash = bytes.fromhex(factor_hash_text)
    if len(factor_hash) != 32:
        raise ValueError("factor name hash must be SHA-256")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with temporary.open("wb") as handle:
        handle.write(
            HEADER.pack(
                MAGIC,
                VERSION,
                ENDIAN_MARKER,
                50,
                128,
                2,
                len(TENSOR_ORDER),
                1.0e-5,
                len(names),
                bytes.fromhex(checkpoint_hash),
                factor_hash,
            )
        )
        for name in names:
            handle.write(pack_string(name))
        for name in TENSOR_ORDER:
            value = tensors[name]
            handle.write(pack_string(name))
            handle.write(struct.pack("<B", value.ndim))
            for dimension in value.shape:
                handle.write(struct.pack("<I", int(dimension)))
            raw = value.astype("<f4", copy=False).tobytes(order="C")
            handle.write(struct.pack("<Q", len(raw)))
            handle.write(raw)
        handle.flush()
    temporary.replace(output)

    golden_path = output.with_name(output.stem + ".golden.bin")
    continuous_stage = root / "golden/continuous_model_stages.npz"
    if continuous_stage.exists():
        stage = np.load(continuous_stage, allow_pickle=False)
        raw_input = np.asarray(stage["raw_input"])[0]
        predictions = np.asarray(stage["prediction"])[0]
        golden_groups = [(0, raw_input[:2], predictions[:2])]
    else:
        stage = np.load(root / "golden/model_stage_outputs.npz", allow_pickle=False)
        index = json.loads((root / "golden/model_stage_index.json").read_text())
        golden_groups = []
        for stock_order in range(5):
            rows = [
                row for row in index
                if int(row["stock_order"]) == stock_order
                and int(row["row_in_stock_day"]) in (0, 1)
            ]
            rows.sort(key=lambda row: int(row["row_in_stock_day"]))
            if [int(row["row_in_stock_day"]) for row in rows] != [0, 1]:
                raise ValueError(f"missing first two model-stage rows for stock {stock_order}")
            stage_rows = [int(row["stage_row"]) for row in rows]
            golden_groups.append(
                (stock_order, stage["raw_input"][stage_rows], stage["cpu_prediction"][stage_rows])
            )
    write_golden_fixture(golden_path, golden_groups)

    metadata = {
        "format": "t0.mix153060.sze-ha3.v1",
        "binary": str(output),
        "binary_sha256": sha256(output),
        "golden_fixture": str(golden_path),
        "golden_fixture_sha256": sha256(golden_path),
        "source_bundle": root.name,
        "checkpoint_sha256": checkpoint_hash,
        "state_dict_sha256": sha256(root / "model/state_dict.npz"),
        "factor_names_sha256": factor_hash_text,
        "factor_order": names,
        "feature_count": 50,
        "hidden_size": 128,
        "num_layers": 2,
        "layernorm_epsilon": 1.0e-5,
        "external_scaler": False,
        "gru_gate_order": "r,z,n",
        "hidden_state_reset": "instrument-day boundary",
        "prediction_output": "raw float32; no clamp",
        "canonical_backend": "CUDA bfloat16 autocast",
        "diagnostic_backend": "CPU float32",
        "static_input_contract": {
            "free_share": "required by factor_tr_sqrt_positive and flow normalization",
            "history_amount": "five-day average amount used for the turnover sampling threshold",
            "history_volatility_20d": "not used by v0.4" if "factor_history_volatility_20d" not in names else "required",
        },
    }
    metadata_path = output.with_suffix(output.suffix + ".json")
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    return metadata


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-root", required=True, type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("/home/bse-t0/models/mix153060_sze_v04_a3_eff60.bin"),
    )
    parser.add_argument(
        "--full-golden-output",
        type=Path,
        help="optionally export all reference CPU inputs/predictions for native validation",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.bundle_root.resolve()
    names = factor_names(root)
    metadata = write_model(root, args.output.resolve(), names)
    if args.full_golden_output is not None:
        write_full_golden_fixture(root, args.full_golden_output.resolve(), names)
        metadata["full_golden_fixture"] = str(args.full_golden_output.resolve())
        metadata["full_golden_fixture_sha256"] = sha256(args.full_golden_output.resolve())
    metadata_path = args.output.resolve().with_suffix(args.output.resolve().suffix + ".json")
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    print(json.dumps(metadata, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
