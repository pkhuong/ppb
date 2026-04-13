PPB: Pico Protobuf
==================
[![codecov](https://codecov.io/gh/pkhuong/ppb/graph/badge.svg?token=YH1Q5HD3B7)](https://codecov.io/gh/pkhuong/ppb)

PPB is an allocation-free non-recursive lexer for protobuf binary
encoding (v2/v3, no groups); like many protobuf implementations, it
silently discard the extra 6 bits in 10-byte varints (but otherwise
rejects varints longer than 10 bytes).  The PPB interface requires the
entire serialized message to be in a contiguous read-only buffer, and
decodes values to 64-bit values, or as subslices in that buffer.

The contents of the input buffer are untrusted and validated as
needed; other inputs to the library must be zero-initialized on
allocations, except for the `ppb_encoded_tag` arrays, which are
assumed to be constructed correctly (check with `ppb_validate_tag`).
The *spirit* of the design is that even invalid trusted inputs will
not introduce undefined behavior, only unexpected (illogical)
results... but there's no formal support for that claim.

The core pattern is: call `ppb_validate_tags` to confirm the array of
tags to parse has a valid structure, call `ppb_prescan` once to
collect field statistics for preallocation, then call `ppb_lexn` in a
loop to walk the fields.  While gathering statistics and validating
the input, `ppb_prescan` also saves the last value associated with
each tag, which implements exactly the last-write-wins semantics
needed for non-repeated fields.  We thus only need `ppb_lexn` for
repeated fields, and only when `ppb_prescan` reports multiple
occurrences of such a field (packed repeated fields, for example,
usually appear at most once).

The `ppb_lexn` function is always safe to call, even without
`ppb_prescan`. Both functions operate only on toplevel fields; call
them recursively on nested submessages.

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

**`struct ppb_encoded_tag`**: a tag identity created with
`PPB_TAG(uint64_t field_number, enum ppb_wire_type)`.  Callers
assemble a `const struct ppb_encoded_tag[]` array sorted in ascending
order and pass it to `ppb_prescan` / `ppb_lexn`; PPB never writes
through this pointer.  Field number `-1` (i.e., `UINT64_MAX` cast to
the field-number argument) is a catch-all that matches any unknown tag
of a given wire type.

The `tags` array *must be sorted* by `.bits` in ascending order.

**`struct ppb_field`**: mutable per-call state for one decoded field.
After a call to `ppb_prescan`, `field.m` holds aggregate metadata
(occurrence count, total/min/max payload bytes for length-prefixed
fields) and `field.v` holds the last decoded value.  Use `ppb_lexn` to
observe every field occurrence: it updates each `field.v` at most once
per call.  The caller owns the `struct ppb_field[]` array, which is
passed alongside (but separately from) the `tags` array (field `i`
matches tag `i`).

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
    PPB_ERROR_UNSORTED_FIELD_ARR = -1,  /* tags[].bits not strictly ascending */
    PPB_ERROR_SENTINEL_FIELD_ARR = -2,  /* tags[].bits includes < 8 (field number 0 is forbidden in protobuf) */
    PPB_ERROR_TRUNCATED_DATA = -3,  /* message cut short at the end of the `ppb_buf` */
    PPB_ERROR_CORRUPT_VARINT = -4,  /* invalid varint encoding (overlong) */
    PPB_ERROR_CORRUPT_TAG = -5,  /* invalid tag encoding (zero, overlong, or unsupported wire type) */
    PPB_ERROR_LIMIT_EXCEEDED = -6,  /* consumed bytes exceeded hard limit */
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
ptrdiff_t ppb_prescan(struct ppb_buf buf, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*
 * Like `ppb_prescan`, but stops at the first field boundary where bytes
 * consumed >= `limit`.  If consumed bytes exceed `limit`, returns
 * `PPB_ERROR_LIMIT_EXCEEDED`.
 */
ptrdiff_t ppb_prescan_with_hard_limit(struct ppb_buf buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*
 * Like `ppb_prescan`, but stops at the first field boundary where bytes
 * consumed >= `limit`.  Consuming more than `limit` bytes is not an error.
 */
ptrdiff_t ppb_prescan_with_soft_limit(struct ppb_buf buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
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
 * Assumes the tags are sorted and valid (no zero tag);
 * ppb_prescan checks for that.
 */
struct ppb_lexn_ret ppb_lexn(struct ppb_buf *__restrict buf, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*
 * Like `ppb_lexn`, but stops at the first field boundary where bytes
 * consumed >= `limit`.  If consumed bytes exceed `limit`, the returned
 * `status` is `PPB_ERROR_LIMIT_EXCEEDED`.
 */
struct ppb_lexn_ret ppb_lexn_with_hard_limit(struct ppb_buf *__restrict buf, size_t limit,
    size_t num_fields, const struct ppb_encoded_tag *__restrict tags,
    struct ppb_field *__restrict fields, size_t max_lexed_fields);

/*
 * Like `ppb_lexn`, but stops at the first field boundary where bytes
 * consumed >= `limit`.  Consuming more than `limit` bytes is not an error.
 */
struct ppb_lexn_ret ppb_lexn_with_soft_limit(struct ppb_buf *__restrict buf, size_t limit,
    size_t num_fields, const struct ppb_encoded_tag *__restrict tags,
    struct ppb_field *__restrict fields, size_t max_lexed_fields);

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

/*
 * Validates a tag array: checks that all entries have `.bits > 7` (field number 0
 * is forbidden in protobuf) and that the array is strictly sorted ascending.
 * Returns `PPB_OK` on success, or the first error found.
 *
 * Call once on any static tag array before passing it to `ppb_prescan` or `ppb_lexn`.
 * Skipping validation does not cause undefined behaviour, but may produce incorrect results.
 */
enum ppb_error ppb_validate_tags(size_t num_fields, const struct ppb_encoded_tag *tags);
```

Usage pattern
-------------

```c
#include "ppb/ppb.h"

enum { F_NAME = 0, F_ID = 1, F_DATA = 2, NUM_FIELDS };

/* Tags are sorted ascending and never modified after init. */
static const struct ppb_encoded_tag tags[NUM_FIELDS] = {
    [F_NAME] = PPB_TAG(1, PPB_WIRE_LEN),    /* string name = 1 */
    [F_ID]   = PPB_TAG(2, PPB_WIRE_VARINT), /* uint64 id   = 2 */
    [F_DATA] = PPB_TAG(5, PPB_WIRE_LEN),    /* bytes  data = 5 */
};
/* Tags must be sorted: PPB_TAG(1,2) < PPB_TAG(2,0) < PPB_TAG(5,2). */

struct ppb_field fields[NUM_FIELDS] = { 0 };  /* mutable per-call state */

/* 0. Validate tags once (e.g., in main() or a static constructor). */
assert(ppb_validate_tags(NUM_FIELDS, tags) == PPB_OK);

/* 1. Validate and gather stats (for preallocation). */
struct ppb_buf msg = { .buf = wire_bytes, .size = wire_len };
ptrdiff_t scanned = ppb_prescan(msg, NUM_FIELDS, tags, fields, SIZE_MAX);
if ((size_t)scanned != msg.size) { /* error */ }

size_t id_count   = fields[F_ID].m.num_occurrences;
size_t name_bytes = fields[F_NAME].m.total_bytes;  /* for allocation */

/* 2. Lex fields. */
while (msg.size > 0)
{
    const char *old_buf = msg.buf;
    struct ppb_lexn_ret r = ppb_lexn(&msg, NUM_FIELDS, tags, fields, SIZE_MAX);
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

**Byte length limiting**: use `ppb_prescan_with_hard_limit` / `ppb_lexn_with_hard_limit`
when the message length is known to be smaller than the remaining readable region: the
hard limit avoids the end-of-buffer slow paths that would otherwise trigger on every
field, which improves performance.  Use the `_with_soft_limit` variants to implement
chunked processing at approximate boundaries: pass a target chunk size as the limit and
PPB will stop at the first clean field boundary at or past that size, so the chunk
always ends just before another valid field.

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
    make test EXTRA_FLAGS='-fsanitize=undefined,address'  # same with ubsan & asan
    make fuzz           # quick libfuzzer-based run
    ./coverage.sh       # report code (line and branch) coverage for `make test`
    make regen_test     # regenerate golden files (requires protoscope in PATH)

Picoscope-based tests are high value for little effort.  To add a test
case for a valid protobuf message, generate protobuf bytes (e.g.,
encode a protoscope source file to binary) and convert to hex:

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

Formal verification, mostly for memory safety and termination, is done
with Frama-C, with `make wp`. Source formatting with `make format`
uses `clang-format-20`.

The ACSL annotations include `admit`ted properties; we try to confirm
them with unit tests and annotations. The fuzz tests in particular
dynamically test ACSL-proven postconditions, in addition to the usual
property that UBSan and ASan must not flag undefined behaviour or
memory safety issues.

Validate the test suite with `mutants.py`: the script mutates the code
and checks whether the mutation is detected by the test suite.
Undetected mutations may point at gaps in the test suite (or maybe the
mutant is expected or an equivalent formulation, and mutation testing
should be disabled locally).
