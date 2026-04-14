#!/usr/bin/env python3
"""
Seed corpus generator for fuzz/fuzz_ppb.

Writes binary corpus files to the output directory:
  1. All testdata/*.hex and testdata/invalid/*.hex decoded to binary.
  2. Synthetic protobuf messages exercising each schema defined in fuzz_ppb.c.

Wire format quick ref
---------------------

tag: 8 * field_number + wire_type
varint: wire type 0
fixed64: wire type 1
length-prefixed: wire type 2
fixed32: wire type 5

invalid wire types (should still be exercised by fuzzing): 3, 4, 6, 7.

Schemas defined in fuzz_ppb.c
------------------------------
  SPECIFIC_TAGS         (N=4) -- all four wire types at small field numbers:
                                  field 1 VARINT, field 2 I64,
                                  field 3 LEN,    field 4 I32.
  CATCHALL_TAGS         (N=4) -- one catch-all per wire type, no specific fields.
  MIXED_TAGS            (N=6) -- field 1 VARINT + field 3 LEN, plus four catch-alls.
  LARGE_TAGS            (N=6) -- all four wire types at large (2- or 3-byte) tag
                                  numbers: field 100 VARINT, field 500 I64,
                                  field 10000 LEN, field 20000 I32,
                                  plus partial catch-alls (VARINT and LEN only;
                                  I64 and I32 unknowns are silently skipped).

Usage:
  python3 fuzz/gen_corpus.py fuzz/corpus
"""

import math
import os
import struct
import sys
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


def zigzag(value: int) -> int:
    return ((value << 1) ^ (-1 if value < 0 else 0)) % (1 << 64)


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
    """Yield (name, bytes) pairs for synthetic protobuf messages."""
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

    # All four SPECIFIC fields in order (matches four_field_wire[] in tests)
    four_fields = (
        field_varint(1, 150)
        + field_i64(2, 1)
        + field_len(3, b"hello")
        + field_i32(4, 42)
    )
    yield "syn_four_fields", four_fields

    # Repeated VARINT
    yield "syn_f1_repeated", field_varint(1, 1) + field_varint(1, 2) + field_varint(
        1, 3
    )

    # Out of order: field 3 before field 1
    yield "syn_out_of_order", field_len(3, b"first") + field_varint(1, 99)

    # Two-byte tag: field 16
    yield "syn_f16_varint", field_varint(16, 42)

    # Three-byte tag: field 2048
    yield "syn_f2048_varint", field_varint(2048, 7)

    # Four-byte tag: field 262144
    yield "syn_f262144_len", field_len(262144, b"big")

    # Unknown field numbers (not in SPECIFIC_TAGS). catch-all or skip
    yield "syn_f5_varint", field_varint(5, 1)
    yield "syn_f10_len", field_len(10, b"catch")
    yield "syn_f100_i64", field_i64(100, 0xDEADBEEF)
    yield "syn_f100_i32", field_i32(100, 0xCAFE)

    # Known + unknown fields, all four wire types (targets MIXED_TAGS)
    yield "syn_mixed_wire", (
        field_varint(1, 0)
        + field_len(3, b"abc")
        + field_varint(5, 99)
        + field_i64(6, 0)
        + field_len(7, b"x" * 4)
        + field_i32(8, 0xAB)
    )

    # Raw 10-byte varint (UINT64_MAX), no tag
    yield "syn_max_varint_raw", b"\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01"

    # Same value as a tagged field 1 VARINT
    yield "syn_f1_varint_u64max", field_varint(1, 2**64 - 1)

    # Nested message: LEN payload is itself a valid field
    yield "syn_nested_msg", field_len(3, field_varint(1, 42))

    # 20x repeated VARINT
    yield "syn_many_f1", b"".join(field_varint(1, i) for i in range(20))

    # Packed repeated varints in a LEN field
    yield "syn_packed_varints", field_len(
        3, b"".join(encode_varint(i) for i in range(10))
    )

    # Truncated messages
    for cut in [1, 2, 3, 5]:
        if cut < len(four_fields):
            yield f"syn_truncated_{cut}", four_fields[: len(four_fields) - cut]

    # LARGE_TAGS: individual fields
    yield "syn_large_f100_varint_0", field_varint(100, 0)
    yield "syn_large_f100_varint_max", field_varint(100, 2**64 - 1)
    yield "syn_large_f500_i64_0", field_i64(500, 0)
    yield "syn_large_f500_i64_max", field_i64(500, 2**64 - 1)
    yield "syn_large_f10000_len_empty", field_len(10000, b"")
    yield "syn_large_f10000_len_hello", field_len(10000, b"hello")
    yield "syn_large_f20000_i32_0", field_i32(20000, 0)
    yield "syn_large_f20000_i32_max", field_i32(20000, 0xFFFFFFFF)
    # All four large fields in order
    yield "syn_large_all_fields", (
        field_varint(100, 42)
        + field_i64(500, 1)
        + field_len(10000, b"large")
        + field_i32(20000, 7)
    )
    # LARGE_TAGS catch-all paths
    yield "syn_large_catchall_varint", field_varint(1, 99)
    yield "syn_large_catchall_i64", field_i64(2, 0)
    yield "syn_large_catchall_len", field_len(3, b"x")
    yield "syn_large_catchall_i32", field_i32(4, 0)

    # MIXED_TAGS known fields in order
    yield "syn_mixed_in_order", field_varint(1, 1) + field_len(3, b"hello")

    # Repeated known field, one per non-VARINT wire type
    yield "syn_f2_i64_repeated", field_i64(2, 0) + field_i64(2, 1) + field_i64(2, 2)
    yield "syn_f3_len_repeated", (
        field_len(3, b"a") + field_len(3, b"bb") + field_len(3, b"ccc")
    )
    yield "syn_f4_i32_repeated", field_i32(4, 0) + field_i32(4, 1) + field_i32(4, 2)

    # All SPECIFIC fields, fully reversed
    yield "syn_specific_reverse", (
        field_i32(4, 1) + field_len(3, b"x") + field_i64(2, 0) + field_varint(1, 42)
    )

    # Two ascending runs: [3,4] then [1,2]
    yield "syn_specific_two_runs", (
        field_len(3, b"x") + field_i32(4, 1) + field_varint(1, 42) + field_i64(2, 0)
    )

    # Same field number, all four wire types (unknown to SPECIFIC)
    yield "syn_same_fn_all_wire_types", (
        field_varint(9, 42) + field_i64(9, 0) + field_len(9, b"x") + field_i32(9, 1)
    )

    # Known (1-4) + unknown (5), in order
    yield "syn_known_unk_in_order", (
        field_varint(1, 1)
        + field_i64(2, 0)
        + field_len(3, b"x")
        + field_i32(4, 5)
        + field_varint(5, 99)
    )
    # Known + unknown, out of order: 5,4,1,3,2
    yield "syn_known_unk_out_of_order", (
        field_varint(5, 99)
        + field_i32(4, 5)
        + field_varint(1, 1)
        + field_len(3, b"x")
        + field_i64(2, 0)
    )
    # Known + unknown, two ascending runs: [3,4,5] then [1,2]
    yield "syn_known_unk_two_runs", (
        field_len(3, b"x")
        + field_i32(4, 5)
        + field_varint(5, 99)
        + field_varint(1, 1)
        + field_i64(2, 0)
    )
    # Known and unknown interleaved, out of order (targets SPECIFIC): 1,5,2,6,3,7,4
    yield "syn_known_unk_interleaved", (
        field_varint(1, 1)
        + field_varint(5, 99)
        + field_i64(2, 0)
        + field_i64(6, 0)
        + field_len(3, b"x")
        + field_len(7, b"y")
        + field_i32(4, 5)
    )
    # Known and unknown interleaved, ascending (targets LARGE): 1,100,200,500,5000,10000,15000,20000
    yield "syn_known_unk_interleaved_asc", (
        field_varint(1, 99)
        + field_varint(100, 1)
        + field_i64(200, 0)
        + field_i64(500, 0)
        + field_len(5000, b"x")
        + field_len(10000, b"y")
        + field_i32(15000, 42)
        + field_i32(20000, 7)
    )

    # Varint boundary values: min/max for each encoded byte length
    _varint_boundaries = [
        (1, 0, "1b_min"),
        (1, 127, "1b_max"),
        (2, 128, "2b_min"),
        (2, 16383, "2b_max"),
        (3, 16384, "3b_min"),
        (3, 2097151, "3b_max"),
        (4, 2097152, "4b_min"),
        (4, 268435455, "4b_max"),
        (5, 268435456, "5b_min"),
        (5, 34359738367, "5b_max"),
        (6, 34359738368, "6b_min"),
        (6, 4398046511103, "6b_max"),
        (7, 4398046511104, "7b_min"),
        (7, 562949953421311, "7b_max"),
        (8, 562949953421312, "8b_min"),
        (8, 72057594037927935, "8b_max"),
        (9, 72057594037927936, "9b_min"),
        (9, 9223372036854775807, "9b_max"),
        (10, 9223372036854775808, "10b_min"),
        (10, 18446744073709551615, "10b_max"),
    ]
    for _nbytes, val, label in _varint_boundaries:
        yield f"syn_varint_{label}", field_varint(1, val)

    # Zigzag-encoded signed integers
    for signed, label in [
        (-(1 << 63), "int64_min"),
        (-1, "neg1"),
        (0, "zero"),
        (1, "pos1"),
        ((1 << 63) - 1, "int64_max"),
    ]:
        yield f"syn_zigzag_{label}", field_varint(1, zigzag(signed))

    # Large LEN payload (800 bytes)
    yield "syn_f3_len_800", field_len(3, b"x" * 800)

    # Single unknown field for the two wire types not covered above
    yield "syn_unk_i64", field_i64(5, 0)
    yield "syn_unk_i32", field_i32(5, 0)

    # IEEE 754 bit patterns as I32 (float32) and I64 (float64)
    for fval, label in [
        (0.0, "zero"),
        (1.0, "one"),
        (-1.0, "neg_one"),
        (math.inf, "inf"),
        (-math.inf, "neginf"),
        (float("nan"), "nan"),
    ]:
        bits32 = struct.unpack("<I", struct.pack("<f", fval))[0]
        yield f"syn_f4_float_{label}", field_i32(4, bits32)
        bits64 = struct.unpack("<Q", struct.pack("<d", fval))[0]
        yield f"syn_f2_double_{label}", field_i64(2, bits64)

    # 7-10 byte varint tags (field numbers large enough to need that many bytes)
    for nbytes, fn in [
        (7, 2**39),  # tag varint = 2**42
        (8, 2**46),  # tag varint = 2**49
        (9, 2**53),  # tag varint = 2**56
        (10, 2**60),  # tag varint = 2**63
    ]:
        yield f"syn_tag_{nbytes}byte", encode_tag(fn, WIRE_VARINT) + encode_varint(0)

    # Unsupported wire types (3=SGROUP, 4=EGROUP, 6/7=reserved)
    for wt in [3, 4, 6, 7]:
        yield f"syn_wire_type_{wt}", encode_tag(1, wt)
    # Same, preceded by a valid field
    for wt in [3, 4, 6, 7]:
        yield f"syn_valid_then_wire_type_{wt}", field_varint(1, 42) + encode_tag(2, wt)


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

    # Locate repo root
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
