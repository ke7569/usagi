#!/usr/bin/env python3
"""Convert the reference NPZ state dict to the native SSEMODL1 artifact.

NumPy is used only during offline artifact preparation. The live strategy
loads the resulting fixed-layout binary in sse_model_runtime.cpp and never
imports PyTorch.
"""

import argparse
import struct

import numpy as np


MAGIC = b"SSEMODL1"
FACTOR_NAMES_SHA256 = bytes.fromhex(
    "24fd61f8c498278dd67db7f183aa4846ae50078143bbd358958299f8817db089")
TENSORS = (
    ("proj.weight", (128, 50)), ("proj.bias", (128,)),
    ("feature_layers.0.weight", (512, 50)), ("feature_layers.0.bias", (512,)),
    ("feature_layers.2.weight", (256, 512)), ("feature_layers.2.bias", (256,)),
    ("feature_layers.4.weight", (128, 256)), ("feature_layers.4.bias", (128,)),
    ("gru.weight_ih_l0", (192, 128)), ("gru.weight_hh_l0", (192, 64)),
    ("gru.bias_ih_l0", (192,)), ("gru.bias_hh_l0", (192,)),
    ("res_gru.weight", (64, 128)), ("res_gru.bias", (64,)),
    ("ln.weight", (64,)), ("ln.bias", (64,)),
    ("prediction_head.0.weight", (8, 64)), ("prediction_head.0.bias", (8,)),
    ("prediction_head.2.weight", (1, 8)), ("prediction_head.2.bias", (1,)),
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", help="reference state_dict.npz")
    parser.add_argument("output", help="native SSEMODL1 artifact")
    args = parser.parse_args()
    source = np.load(args.input)
    with open(args.output, "wb") as target:
        target.write(MAGIC)
        target.write(struct.pack("<I", 1))
        target.write(FACTOR_NAMES_SHA256)
        for name, shape in TENSORS:
            if name not in source.files:
                raise ValueError("missing tensor: %s" % name)
            tensor = np.asarray(source[name], dtype=np.float32)
            if tensor.shape != shape:
                raise ValueError("tensor %s shape %s != %s" % (name, tensor.shape, shape))
            if not np.isfinite(tensor).all():
                raise ValueError("tensor %s contains non-finite values" % name)
            target.write(tensor.tobytes(order="C"))
    print("converted %d tensors to %s" % (len(TENSORS), args.output))


if __name__ == "__main__":
    main()
