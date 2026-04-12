#!/usr/bin/env python3
"""
Seed corpus generator for fuzz/fuzz_ppb.

Writes binary corpus files to the output directory:
  1. All testdata/*.hex and testdata/invalid/*.hex decoded to binary.
  2. Synthetic protobuf messages exercising the field definitions used in
     fuzz_ppb.c (fields 1-4 with wire types VARINT/I64/LEN/I32, catch-all
     field numbers, multi-byte tags, edge-case varints).

Usage:
  python3 fuzz/gen_corpus.py fuzz/corpus
"""

import os
import sys
import struct
import glob


WIRE_VARINT = 0
WIRE_I64 = 1
WIRE_LEN = 2
WIRE_I32 = 5


def encode_varint(value: int) -> bytes:
    """Encode a non-negative integer as a protobuf varint."""
    if value < 0:
        raise ValueError("varint must be non-negative")
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            break
    return bytes(out)


def encode_tag(field_number: int, wire_type: int) -> bytes:
    return encode_varint((field_number << 3) | wire_type)


def field_varint(field_number: int, value: int) -> bytes:
    return encode_tag(field_number, WIRE_VARINT) + encode_varint(value)


def field_i64(field_number: int, value: int) -> bytes:
    return encode_tag(field_number, WIRE_I64) + struct.pack(
        "<Q", value & 0xFFFFFFFFFFFFFFFF
    )


def field_len(field_number: int, payload: bytes) -> bytes:
    return encode_tag(field_number, WIRE_LEN) + encode_varint(len(payload)) + payload


def field_i32(field_number: int, value: int) -> bytes:
    return encode_tag(field_number, WIRE_I32) + struct.pack("<I", value & 0xFFFFFFFF)


def synthetic_entries():
    """
    Yield (name, bytes) pairs for synthetic protobuf messages.

    Field layout mirrors fuzz_ppb.c:
      field 1 VARINT, field 2 I64, field 3 LEN, field 4 I32
    """
    # Empty message
    yield "syn_empty", b""

    # Field 1 VARINT with various values
    for val, label in [
        (0, "0"),
        (1, "1"),
        (150, "150"),
        (127, "127"),
        (128, "128"),
        (0x7FFFFFFF, "i32max"),
        (0xFFFFFFFF, "u32max"),
        (2**63 - 1, "i64max"),
        (2**64 - 1, "u64max"),
    ]:
        yield f"syn_f1_varint_{label}", field_varint(1, val)

    # Field 2 I64
    for val, label in [(0, "0"), (1, "1"), (2**64 - 1, "max")]:
        yield f"syn_f2_i64_{label}", field_i64(2, val)

    # Field 3 LEN with various payloads
    yield "syn_f3_len_empty", field_len(3, b"")
    yield "syn_f3_len_hello", field_len(3, b"hello")
    yield "syn_f3_len_128", field_len(3, b"x" * 128)
    yield "syn_f3_len_varint_payload", field_len(
        3, encode_varint(150) + encode_varint(300) + encode_varint(0)
    )

    # Field 4 I32
    for val, label in [(0, "0"), (42, "42"), (0xFFFFFFFF, "max")]:
        yield f"syn_f4_i32_{label}", field_i32(4, val)

    # All four fields together (matches four_field_wire[] in test suite)
    four_fields = (
        field_varint(1, 150)
        + field_i64(2, 1)
        + field_len(3, b"hello")
        + field_i32(4, 42)
    )
    yield "syn_four_fields", four_fields

    # Repeated field 1
    yield "syn_f1_repeated", field_varint(1, 1) + field_varint(1, 2) + field_varint(
        1, 3
    )

    # Fields out of order (field 3 before field 1)
    yield "syn_out_of_order", field_len(3, b"first") + field_varint(1, 99)

    # Two-byte tag: field 16
    yield "syn_f16_varint", field_varint(16, 42)

    # Three-byte tag: field 2048
    yield "syn_f2048_varint", field_varint(2048, 7)

    # Four-byte tag: field 262144
    yield "syn_f262144_len", field_len(262144, b"big")

    # High field number to hit catch-all (field 5 is not in SPECIFIC_TAGS)
    yield "syn_f5_varint", field_varint(5, 1)
    yield "syn_f10_len", field_len(10, b"catch")
    yield "syn_f100_i64", field_i64(100, 0xDEADBEEF)
    yield "syn_f100_i32", field_i32(100, 0xCAFE)

    # Multiple wire types for same virtual field number (distinct tags)
    yield "syn_mixed_wire", (
        field_varint(1, 0)
        + field_len(3, b"abc")
        + field_varint(5, 99)  # catch-all VARINT
        + field_i64(6, 0)  # catch-all I64
        + field_len(7, b"x" * 4)  # catch-all LEN
        + field_i32(8, 0xAB)  # catch-all I32
    )

    # Maximum-length varint (10 bytes, encodes UINT64_MAX)
    max_varint = b"\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01"
    yield "syn_max_varint_raw", max_varint  # raw varint, no tag

    # Varint-encoded UINT64_MAX as field 1 value
    yield "syn_f1_varint_u64max", field_varint(1, 2**64 - 1)

    # Nested message in field 3 (field 1 varint inside)
    nested = field_varint(1, 42)
    yield "syn_nested_msg", field_len(3, nested)

    # Many repeated fields
    many = b"".join(field_varint(1, i) for i in range(20))
    yield "syn_many_f1", many

    # Packed repeated varints in field 3
    packed = b"".join(encode_varint(i) for i in range(10))
    yield "syn_packed_varints", field_len(3, packed)

    # Truncated messages (useful seeds for truncation error paths)
    full = four_fields
    for cut in [1, 2, 3, 5]:
        if cut < len(full):
            yield f"syn_truncated_{cut}", full[: len(full) - cut]


def testdata_entries(repo_root: str):
    """
    Yield (name, bytes) pairs for every *.hex file under testdata/.
    """
    patterns = [
        os.path.join(repo_root, "testdata", "*.hex"),
        os.path.join(repo_root, "testdata", "invalid", "*.hex"),
    ]
    for pattern in patterns:
        for path in sorted(glob.glob(pattern)):
            base = os.path.splitext(os.path.basename(path))[0]
            # Determine prefix so testdata/ and invalid/ don't collide
            if "invalid" in os.path.dirname(path):
                name = f"td_invalid_{base}"
            else:
                name = f"td_{base}"
            try:
                with open(path) as f:
                    hex_text = f.read().replace("\n", "").replace(" ", "").strip()
                    # Handle files ending with ~ that slipped through
                    if not hex_text:
                        continue
                    data = bytes.fromhex(hex_text)
                yield name, data
            except (ValueError, OSError) as e:
                print(f"  warning: skipping {path}: {e}", file=sys.stderr)


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output-dir>", file=sys.stderr)
        sys.exit(1)

    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)

    # Locate repo root (two levels up from this script: fuzz/ → repo root)
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    written = 0
    seen_names = set()

    def write_entry(name, data):
        nonlocal written
        if name in seen_names:
            return
        seen_names.add(name)
        out_path = os.path.join(out_dir, name)
        with open(out_path, "wb") as f:
            f.write(data)
        written += 1

    for name, data in testdata_entries(repo_root):
        write_entry(name, data)

    for name, data in synthetic_entries():
        write_entry(name, data)

    print(f"Wrote {written} corpus files to {out_dir}/")


if __name__ == "__main__":
    main()
