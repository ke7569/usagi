#!/usr/bin/env python3
"""Safely extract the mix153060 .tar.zst handoff without a zstd CLI."""

import argparse
import ctypes
import hashlib
import io
import os
import tarfile
from pathlib import Path


EXPECTED_ARCHIVE_SHA256 = "60d0f8d7e3a5c124d47fa79244e623994962961468ed0ebabccc3bd90aa78cf8"
ZSTD_CONTENTSIZE_ERROR = (1 << 64) - 1
ZSTD_CONTENTSIZE_UNKNOWN = (1 << 64) - 2


class ZstdInBuffer(ctypes.Structure):
    _fields_ = [("src", ctypes.c_void_p), ("size", ctypes.c_size_t), ("pos", ctypes.c_size_t)]


class ZstdOutBuffer(ctypes.Structure):
    _fields_ = [("dst", ctypes.c_void_p), ("size", ctypes.c_size_t), ("pos", ctypes.c_size_t)]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def decompress(path: Path) -> bytes:
    compressed = path.read_bytes()
    library = ctypes.CDLL("libzstd.so.1")
    library.ZSTD_getFrameContentSize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    library.ZSTD_getFrameContentSize.restype = ctypes.c_uint64
    library.ZSTD_decompress.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    library.ZSTD_decompress.restype = ctypes.c_size_t
    library.ZSTD_isError.argtypes = [ctypes.c_size_t]
    library.ZSTD_isError.restype = ctypes.c_uint
    library.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
    library.ZSTD_getErrorName.restype = ctypes.c_char_p

    source = ctypes.create_string_buffer(compressed)
    output_size = int(library.ZSTD_getFrameContentSize(source, len(compressed)))
    if output_size not in (ZSTD_CONTENTSIZE_ERROR, ZSTD_CONTENTSIZE_UNKNOWN):
        output = ctypes.create_string_buffer(output_size)
        written = int(library.ZSTD_decompress(output, output_size, source, len(compressed)))
        if library.ZSTD_isError(written):
            message = library.ZSTD_getErrorName(written).decode("ascii", errors="replace")
            raise RuntimeError(f"Zstandard decompression failed: {message}")
        if written != output_size:
            raise RuntimeError(f"decompressed {written} bytes, expected {output_size}")
        return output.raw

    # The handoff uses a streaming frame without a content-size field.
    library.ZSTD_createDStream.restype = ctypes.c_void_p
    library.ZSTD_initDStream.argtypes = [ctypes.c_void_p]
    library.ZSTD_initDStream.restype = ctypes.c_size_t
    library.ZSTD_decompressStream.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ZstdOutBuffer),
        ctypes.POINTER(ZstdInBuffer),
    ]
    library.ZSTD_decompressStream.restype = ctypes.c_size_t
    library.ZSTD_freeDStream.argtypes = [ctypes.c_void_p]
    library.ZSTD_freeDStream.restype = ctypes.c_size_t
    stream = library.ZSTD_createDStream()
    if not stream:
        raise RuntimeError("cannot allocate Zstandard stream")
    try:
        initialized = int(library.ZSTD_initDStream(stream))
        if library.ZSTD_isError(initialized):
            raise RuntimeError("cannot initialize Zstandard stream")
        source_ptr = ctypes.cast(source, ctypes.c_void_p)
        input_buffer = ZstdInBuffer(source_ptr, len(compressed), 0)
        chunks = []
        while input_buffer.pos < input_buffer.size:
            output = ctypes.create_string_buffer(1 << 20)
            output_buffer = ZstdOutBuffer(ctypes.cast(output, ctypes.c_void_p), len(output), 0)
            remaining = int(library.ZSTD_decompressStream(stream, ctypes.byref(output_buffer), ctypes.byref(input_buffer)))
            if library.ZSTD_isError(remaining):
                message = library.ZSTD_getErrorName(remaining).decode("ascii", errors="replace")
                raise RuntimeError(f"Zstandard decompression failed: {message}")
            if output_buffer.pos:
                chunks.append(output.raw[: output_buffer.pos])
            if remaining == 0 and input_buffer.pos == input_buffer.size:
                break
            if output_buffer.pos == 0 and input_buffer.pos == input_buffer.size:
                raise RuntimeError("truncated Zstandard stream")
        return b"".join(chunks)
    finally:
        library.ZSTD_freeDStream(stream)


def validate_members(archive: tarfile.TarFile, destination: Path) -> None:
    root = destination.resolve()
    for member in archive.getmembers():
        if member.issym() or member.islnk():
            raise ValueError(f"archive links are not allowed: {member.name}")
        target = (destination / member.name).resolve()
        if os.path.commonpath((str(root), str(target))) != str(root):
            raise ValueError(f"archive path escapes destination: {member.name}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--archive",
        type=Path,
        default=Path("/home/bse-t0/ref/mix153060-20260715.tar.zst"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("/home/bse-t0/src/t0-main/build/mix153060-20260715"),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    archive_path = args.archive.resolve()
    destination = args.output.resolve()
    actual_hash = sha256(archive_path)
    if actual_hash != EXPECTED_ARCHIVE_SHA256:
        raise ValueError(
            f"archive SHA-256 mismatch: expected {EXPECTED_ARCHIVE_SHA256}, got {actual_hash}"
        )
    if destination.exists() and any(destination.iterdir()):
        raise FileExistsError(f"refusing to overwrite non-empty destination: {destination}")
    destination.mkdir(parents=True, exist_ok=True)
    content = decompress(archive_path)
    with tarfile.open(fileobj=io.BytesIO(content), mode="r:") as archive:
        validate_members(archive, destination)
        archive.extractall(destination)
    print(destination / "mix153060")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
