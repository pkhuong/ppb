PPB: Pico Protobuf
==================

PPB is an allocation-free non-recursive lexer for protobuf binary
encoding (v2/v3, no groups).  It requires the entire serialized
message to be in a contiguous read-only buffer, and decodes values to
64-bit values, or as subslices in that buffer.

The core pattern is: call `ppb_prescan` once to validate the input and
collect field statistics for preallocation, then call `ppb_lexn` in a
loop to walk the fields.  While validating, `ppb_prescan` also saves
the last value associated with each tag, which implements exactly the
last-write-wins semantics needed for non-repeated fields.  We thus
only need `ppb_lexn` for repeated fields, and only when `ppb_prescan`
reports multiple occurrences of such a field (packed repeated fields,
for example, usually appear at most once).

The `ppb_lexn` function is always safe to call, even without
`ppb_prescan`: `ppb_lexn` performs its own redundant validation *of
message bytes* on the fly (`ppb_prescan` also validates the structure
of the `fields[]` array).  Both functions operate only on toplevel
fields; call them recursively on nested submessages.

This usage pattern lets the calling program choose how to handle
nesting and variable-length values.  Both `ppb_prescan` and
`ppb_lexn`'s runtimes scale with the number of toplevel fields, not
the total number of message bytes, so recursive processing time
remains linear in the number of message bytes.

Quick start
-----------

Build the library with `make` to produce `build/libppb.a` and
`build/libppb.so`.  Link against `build/libppb.a` (or
`build/libppb.so`) and include `include/ppb/ppb.h`.  The library has
no external dependencies and requires C11 (`-std=c11`).

Concepts
--------

**`struct ppb_buf`**: a read-only byte slice `{ const void *buf; size_t size; }`.
PPB never writes through the `buf` pointer, but does update the struct
in place and construct subslices for length-prefixed payloads
(variable-length values).

**`struct ppb_field`**: one entry in a caller-owned array that
describes a field that should be decoded.  Set `field.tag` with
`PPB_TAG(uint64_t field_number, enum ppb_wire_type)` before calling
PPB.  After a call to `ppb_prescan`, `field.m` holds aggregate
metadata (occurrence count, total/min/max payload bytes for
length-prefixed fields) and `field.v` holds the last decoded value.
Use `ppb_lexn` to observe every field occurrence: it updates each
`field.v` at most once per call.

The field array *must be sorted* by `tag.bits` in ascending order.  Field
number `-1` (i.e., `UINT64_MAX` cast to the field-number argument) is a
catch-all that matches any unknown tag of a given wire type.

API (include/ppb/ppb.h)
-----------------------

**Complexity**: Helper functions `ppb_zag` and `ppb_decode_varint`
have a bounded runtime (asymptotically constant).  The `ppb_prescan`
function runs in Θ(m + n log m), while `ppb_lexn` runs in Θ(n log m),
where n is the number of toplevel fields consumed and m is
`num_fields`.  Runtime does *not* scale with payload sizes of
length-prefixed fields -- that's what makes recursive descent on
nested submessages practical.

```c
/*
 * All PPB public functions use the same error enum.  Error codes are
 * always strictly negative.
 */
enum ppb_error
{
    PPB_OK = 0,
    PPB_ERROR_UNSORTED_FIELD_ARR = -1,  /* fields[].tag.bits not strictly ascending */
    PPB_ERROR_SENTINEL_FIELD_ARR = -2,  /* fields[].tag.bits includes 0 (invalid in protobuf) */
    PPB_ERROR_TRUNCATED_DATA = -3,  /* message cut short at the end of the `ppb_buf` */
    PPB_ERROR_CORRUPT_VARINT = -4,  /* invalid varint encoding (overlong) */
    PPB_ERROR_CORRUPT_TAG = -5,  /* invalid tag encoding (zero, overlong, or unsupported wire type) */
};

/*
 * Traverses `buf` until it's lexed up to `max_lexed_fields` toplevel
 * fields.  This traversal is relatively cheap, and can be useful to
 * preallocate storage.
 *
 * Returns the number of bytes traversed, until the end of `buf`, or
 * just before the first byte of the `max_lexed_fields`th field.
 *
 * Updates `fields[].m` with aggregate metadata for all the fields
 * seen, and populates `fields[].v` with the last value seen for each
 * field, if any.
 *
 * A non-negative value is the total number of bytes summarized (equals
 * buf.size on success).
 *
 * A negative value is a ppb_error.
 */
ptrdiff_t ppb_prescan(struct ppb_buf buf, size_t num_fields, struct ppb_field fields[__restrict num_fields],
    size_t max_lexed_fields);

/*
 * A `ppb_lexn_ret` describes a range of indices in `fields` that
 * contain all fields decoded in that call to `ppb_lexn` (but may
 * include indices for irrelevant fields), and an error code or
 * `PPB_OK`.
 *
 * The range is `fields[first_field .. first_field + field_range)`
 * with `first_field + field_range <= num_fields`, and `field_range`
 * saturating at `UINT32_MAX`: when `field_range == UINT32_MAX`, the
 * range is instead `fields[first_field .. num_fields)`.
 */
struct ppb_lexn_ret
{
    size_t first_field;
    uint32_t field_range;
    enum ppb_error status;
};

/*
 * Consumes from `buf` until it's lexed up to `max_lexed_fields`
 * toplevel fields.  May stop early, but only after at least 1 field,
 * or on error/end-of-buf.
 *
 * This function may decode multiple fields at a time, but only in
 * strictly ascending order, and always stops before a non-monotonic
 * (repeated or decreasing) tag or after an unknown field.  This
 * guarantees that we can always recover the order of the fields on
 * the wire, and that we always see every field value.  Call
 * `ppb_lexn` in a loop to process all fields in a message, in batches
 * of strictly monotonically increasing fields.
 *
 * Updates `fields[].v` in place, and sets `ptr` to the decoded
 * field's first byte in `buf` (i.e., where the tag varint begins);
 * check if we have just decoded the field by comparing its `ptr` with
 * the initial value of `buf.buf`.
 *
 * Assumes the fields are sorted and valid (no zero field);
 * ppb_prescan checks for that.
 */
struct ppb_lexn_ret ppb_lexn(struct ppb_buf *__restrict buf, size_t num_fields,
    struct ppb_field fields[__restrict num_fields], size_t max_lexed_fields);

/*
 * Decodes zigzag-encoded sint32 and sint64 values.
 *
 * A 32-bit unsigned int value will decode to an `int32_t`.
 */
static inline int64_t
ppb_zag(uint64_t x)
{
    return (int64_t)((x >> 1) ^ -(x & 1));
}

/*
 * Attempts to consume one varint from `buf`.
 *
 * Returns the decoded varint, or 0 on error.
 *
 * Confirm whether there was an error by looking at `error` (initially
 * zero): it will be strictly negative on error, and zero on success.
 */
uint64_t ppb_decode_varint(struct ppb_buf *__restrict buf, enum ppb_error *__restrict error);
```

Usage pattern
-------------

```c
#include "ppb/ppb.h"

enum { F_NAME = 0, F_ID = 1, F_DATA = 2, NUM_FIELDS };

struct ppb_field fields[NUM_FIELDS] = {
    [F_NAME] = { .tag = PPB_TAG(1, PPB_WIRE_LEN)    },  /* string name = 1 */
    [F_ID]   = { .tag = PPB_TAG(2, PPB_WIRE_VARINT) },  /* uint64 id   = 2 */
    [F_DATA] = { .tag = PPB_TAG(5, PPB_WIRE_LEN)    },  /* bytes  data = 5 */
};
/* Tags must be sorted: PPB_TAG(1,2) < PPB_TAG(2,0) < PPB_TAG(5,2). */

/* 1. Validate and gather stats (for preallocation). */
struct ppb_buf msg = { .buf = wire_bytes, .size = wire_len };
ptrdiff_t scanned = ppb_prescan(msg, NUM_FIELDS, fields, SIZE_MAX);
if ((size_t)scanned != msg.size) { /* error */ }

size_t id_count   = fields[F_ID].m.num_occurrences;
size_t name_bytes = fields[F_NAME].m.total_bytes;  /* for allocation */

/* 2. Lex fields. */
while (msg.size > 0)
{
    const char *old_buf = msg.buf;
    struct ppb_lexn_ret r = ppb_lexn(&msg, NUM_FIELDS, fields, SIZE_MAX);
    if (r.status != PPB_OK) { /* error */ }

    /* r.field_range <= NUM_FIELDS - r.first_field. */
    for (size_t i = r.first_field; i < r.first_field + r.field_range; i++)
    {
        if ((const char *)fields[i].v.ptr < (const char *)old_buf ||
            (const char *)fields[i].v.ptr >= (const char *)msg.buf)
        {
            continue;
        }

        switch (i)
        {
        case F_NAME:
            use_string(fields[i].v.payload);
            break;
        case F_ID:
            use_id(fields[i].v.u64);
            break;
        case F_DATA:
            use_bytes(fields[i].v.payload);
            break;
        }
    }
}
```

**Detecting decoded fields**: save `buf.buf` before a `ppb_lexn` call.
After the call, `old_buf <= field.v.ptr < buf.buf` iff that field was
decoded in this call.  Fields PPB did not see are left untouched
(e.g., zero from initialization).

**Nested submessages**: when a `PPB_WIRE_LEN` field is a submessage, pass
`field.v.payload` as the `buf` argument to a fresh `ppb_prescan` / `ppb_lexn`
pair.

**Repeated fields**: `ppb_lexn` stops as soon as a tag is non-monotonic
(repeated or lower than the previous tag), so each call yields at most one
occurrence of any given field.  `field.v` holds the value decoded in this
call; the outer loop naturally delivers each occurrence in the encoded order.

**Catch-all entries**: use `PPB_TAG(-1, wire_type)` to match any field of a
given wire type that wasn't matched by a specific entry.  Catch-all entries
sort after all specific entries.

**Value fields** in `struct ppb_field_value`:
- `u64`: varint or raw bytes of a fixed64 field (little-endian).
- `u32`: low 32 bits (for fixed32 fields), little-endian.
- `d` / `f`: double / float (assumes little-endian host).
- `b[8]`: raw bytes of the fixed-width field.
- `payload`: `struct ppb_buf` pointing at the payload of a `PPB_WIRE_LEN` field.
- `ptr`: pointer to the field's tag byte in the original buffer (useful for catch-all).

**Zigzag**: call `ppb_zag(field.v.u64)` to decode `sint32` / `sint64` values.

Example tool
------------

`examples/picoscope.c` is a self-contained binary that disassembles a
binary protobuf file to text, similarly to
[protoscope](https://github.com/protocolbuffers/protoscope).  Build it
with `make`, then:

    ./build/picoscope file.pb
    ./build/picoscope -p file.pb   # protoscope-compatible output

Development and validation
--------------------------

The test suite runs unit tests and comparisons against
[protoscope](https://github.com/protocolbuffers/protoscope):

    make test           # run unit tests + golden tests
    make regen_test     # regenerate golden files (requires protoscope in PATH)

To add a valid test case, encode a protoscope source file to binary and
convert to hex:

    echo '1: 42  2: {"hello"}' | protoscope -s | xxd -p | tr -d '\n' \
        > testdata/my-test.hex
    make regen_test   # writes testdata/my-test.expected

The test suite also automatically truncates every valid message by 1–32
bytes and checks that picoscope either errors out or produces a prefix of
the expected output.

To add an invalid test case, place the `.hex` file in `testdata/invalid/`
and run `make regen_test`; it captures the expected error message into a
`.expected-error` file.

If protoscope is not in PATH, pass it explicitly:

    make regen_test PROTOSCOPE=/path/to/protoscope

The unit tests cover `ppb_zag`, varint decoding, `ppb_prescan` and
`ppb_lexn` directly: field-array validation, metadata accumulation,
`max_lexed_fields` early-exit, monotonic-run batching, unknown-field
skipping, and error detection for malformed input.  The golden tests
exercise `picoscope` end-to-end across all wire types, multi-byte
varint and tag encodings, repeated and nested fields.  The
invalid-input suite checks that corrupt or truncated data is rejected
with the correct error.

Formal verification, mostly for memory safety, is done with Frama-C,
with `make wp`. Source formatting with `make format` uses
`clang-format-20`.
