#!/usr/bin/env python3
"""Convert the handoff's torch state-dict zip into an SSESGRU1 artifact.

This converter intentionally uses only the Python standard library.  It can
read the ``model.pt`` state-dict export (and the larger ``best.pt`` checkpoint)
without importing PyTorch, then writes the exact FP32 tensors needed by the
native C++ runtime.  PyTorch is therefore an offline/reference dependency,
never a serving dependency.
"""

from __future__ import print_function

import argparse
import json
import pickle
import struct
import zipfile


MAGIC = b"SSESGRU1"
VERSION = 1
HIDDEN_SIZE = 64


class _StorageRef(object):
    def __init__(self, key, size):
        self.key = str(key)
        self.size = int(size)


class _TensorRef(object):
    def __init__(self, storage, offset, shape, stride):
        self.storage = storage
        self.offset = int(offset)
        self.shape = tuple(int(x) for x in shape)
        self.stride = tuple(int(x) for x in stride)


class _DummyPath(object):
    def __new__(cls, *parts):
        return "/".join(str(x) for x in parts)


class _Unpickler(pickle.Unpickler):
    def persistent_load(self, pid):
        if not isinstance(pid, tuple) or len(pid) < 5 or pid[0] != "storage":
            raise ValueError("unsupported persistent pickle id: %r" % (pid,))
        return _StorageRef(pid[2], pid[4])

    def find_class(self, module, name):
        if module == "torch._utils" and name.startswith("_rebuild_tensor"):
            return lambda storage, offset, shape, stride, *unused: _TensorRef(
                storage, offset, shape, stride)
        if module == "torch._utils" and name == "_rebuild_parameter":
            return lambda value, *unused: value
        if module == "torch" and name.endswith("Storage"):
            return type(str(name), (object,), {})
        if module.startswith("pathlib"):
            return _DummyPath
        return pickle.Unpickler.find_class(self, module, name)


def _product(shape):
    result = 1
    for value in shape:
        result *= int(value)
    return result


def _contiguous_stride(shape):
    stride = [0] * len(shape)
    running = 1
    for index in range(len(shape) - 1, -1, -1):
        stride[index] = running
        running *= shape[index]
    return tuple(stride)


def _tensor_bytes(ref, storage_bytes):
    count = _product(ref.shape)
    expected_stride = _contiguous_stride(ref.shape)
    if ref.stride == expected_stride:
        start = ref.offset * 4
        end = start + count * 4
        payload = storage_bytes[start:end]
        if len(payload) != count * 4:
            raise ValueError("tensor storage slice is truncated")
        return payload

    # The supplied exports are contiguous, but retaining this path makes the
    # converter safe for a future state-dict containing a regular view.
    values = []
    for linear in range(count):
        remaining = linear
        source_index = ref.offset
        for axis in range(len(ref.shape) - 1, -1, -1):
            coordinate = remaining % ref.shape[axis]
            remaining //= ref.shape[axis]
            source_index += coordinate * ref.stride[axis]
        values.append(struct.unpack_from("<f", storage_bytes, source_index * 4)[0])
    return struct.pack("<%df" % len(values), *values)


def _load_state_dict(checkpoint_path):
    with zipfile.ZipFile(checkpoint_path, "r") as archive:
        data_entries = [name for name in archive.namelist()
                        if name.endswith("/data.pkl")]
        if len(data_entries) != 1:
            raise ValueError("expected exactly one torch data.pkl in %s" % checkpoint_path)
        data_name = data_entries[0]
        prefix = data_name[:-len("data.pkl")]
        root = _Unpickler(archive.open(data_name, "r")).load()
        if not isinstance(root, dict) or "model" not in root:
            raise ValueError("checkpoint does not contain a model state_dict")
        state_dict = root["model"]
        if not hasattr(state_dict, "items"):
            raise ValueError("checkpoint model entry is not a mapping")

        storage_cache = {}

        def storage_bytes(storage):
            if storage.key not in storage_cache:
                entry = prefix + "data/" + storage.key
                payload = archive.read(entry)
                if len(payload) != storage.size * 4:
                    raise ValueError("storage %s byte count mismatch" % storage.key)
                storage_cache[storage.key] = payload
            return storage_cache[storage.key]

        result = {}
        for name, value in state_dict.items():
            if not isinstance(value, _TensorRef):
                raise ValueError("state_dict entry %s is not a tensor" % name)
            result[str(name)] = (value.shape, _tensor_bytes(value, storage_bytes(value.storage)))
        return result


def _expected(feature_count):
    return [
        ("proj.weight", (128, feature_count)),
        ("proj.bias", (128,)),
        ("feature_layers.0.weight", (512, feature_count)),
        ("feature_layers.0.bias", (512,)),
        ("feature_layers.2.weight", (256, 512)),
        ("feature_layers.2.bias", (256,)),
        ("feature_layers.4.weight", (128, 256)),
        ("feature_layers.4.bias", (128,)),
        ("gru.weight_ih_l0", (192, 128)),
        ("gru.weight_hh_l0", (192, 64)),
        ("gru.bias_ih_l0", (192,)),
        ("gru.bias_hh_l0", (192,)),
        ("res_gru.weight", (64, 128)),
        ("res_gru.bias", (64,)),
        ("ln.weight", (64,)),
        ("ln.bias", (64,)),
        ("prediction_head.0.weight", (8, 64)),
        ("prediction_head.0.bias", (8,)),
        ("prediction_head.2.weight", (1, 8)),
        ("prediction_head.2.bias", (1,)),
    ]


def _validate_scaler(path, feature_count):
    with open(path, "r") as handle:
        payload = json.load(handle)
    if not isinstance(payload.get("mean"), list) or not isinstance(payload.get("scale"), list):
        raise ValueError("scaler must contain mean and scale arrays")
    if len(payload["mean"]) != feature_count or len(payload["scale"]) != feature_count:
        raise ValueError("scaler feature count mismatch")
    names = payload.get("feature_names")
    if names is not None and len(names) != feature_count:
        raise ValueError("scaler feature_names count mismatch")


def convert(checkpoint_path, scaler_path, output_path, feature_count):
    _validate_scaler(scaler_path, feature_count)
    state = _load_state_dict(checkpoint_path)
    expected = _expected(feature_count)
    if set(state.keys()) != set(name for name, unused in expected):
        missing = sorted(set(name for name, unused in expected) - set(state.keys()))
        extra = sorted(set(state.keys()) - set(name for name, unused in expected))
        raise ValueError("state_dict keys mismatch; missing=%r extra=%r" % (missing, extra))
    with open(output_path, "wb") as output:
        output.write(MAGIC)
        output.write(struct.pack("<IIIII", VERSION, feature_count,
                                 HIDDEN_SIZE, len(expected), 0))
        for name, shape in expected:
            actual_shape, payload = state[name]
            if tuple(actual_shape) != tuple(shape):
                raise ValueError("%s shape %r expected %r" % (name, actual_shape, shape))
            encoded = name.encode("ascii")
            if len(encoded) > 255:
                raise ValueError("tensor name too long: %s" % name)
            output.write(struct.pack("<H", len(encoded)))
            output.write(encoded)
            output.write(struct.pack("<B", len(shape)))
            output.write(b"\0\0\0")
            for dimension in shape:
                output.write(struct.pack("<I", dimension))
            output.write(struct.pack("<Q", len(payload)))
            output.write(payload)
    return output_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", required=True,
                        help="model.pt or best.pt from the handoff")
    parser.add_argument("--scaler", required=True,
                        help="baseline.json or auction59.json")
    parser.add_argument("--feature-count", required=True, type=int,
                        choices=(36, 95))
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    path = convert(args.checkpoint, args.scaler, args.output, args.feature_count)
    print("wrote %s feature_count=%d" % (path, args.feature_count))


if __name__ == "__main__":
    main()
