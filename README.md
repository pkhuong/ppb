PPB: Pico Protobuf
==================
[![codecov](https://codecov.io/gh/pkhuong/ppb/graph/badge.svg?token=YH1Q5HD3B7)](https://codecov.io/gh/pkhuong/ppb)
<a href="https://scan.coverity.com/projects/pkhuong-ppb">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/33051/badge.svg"/>
</a>

PPB is an allocation-free non-recursive lexer for protobuf binary
encoding (v2/v3, no groups); like many protobuf implementations, it
silently discards bits 1-6 of byte 10 in 10-byte varints (only bit 0
of byte 10 reaches bit 63 of the result) and rejects longer varints
with `PPB_ERROR_CORRUPT_VARINT`.  The PPB interface requires the
entire serialized message to be in a contiguous read-only buffer, and
decodes values to 64-bit values, or as subslices in that buffer.

The contents of the input buffer are untrusted and validated as
needed; other inputs to the library must be zero-initialized on
allocations, except for the `ppb_encoded_tag` arrays, which are
assumed to be constructed correctly (check with `ppb_validate_tags`).
Even invalid trusted inputs (an unsorted tag array, non-zero-initialized
`fields[]`, etc.) cause *at worst* surprising results or, in builds
with assertions enabled, assertion failures; never memory unsafety,
undefined behavior, non-termination, or successful return without
progress (for `lexn`).  [Frama-C's WP](https://www.frama-c.com/fc-plugins/wp.html)
discharges memory safety, termination, and progress regardless of the
contents of input buffers and arrays (trusted or otherwise).  See
[SECURITY.md](SECURITY.md) for the full two-tier breakdown of
trusted-input preconditions and what's considered out of scope, and
[AUDITING.md](AUDITING.md) for the verification recipes a skeptical
reader can run against each claim independently.

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

Compiler support
----------------

PPB targets C11 with GCC extensions; CI tests GCC and clang builds on
64-bit (x86-64, aarch64) and 32-bit (i686) little-endian platforms,
and on big-endian 64-bit s390x (via QEMU).

MSVC is not supported (but patches are welcome).  I'd also consider
patches for strict C11 compliance, it just wouldn't be useful for me.

Quick start
-----------

Build the library with `make` to produce `build/libppb.a` and
`build/libppb.so`.  Link against `build/libppb.a` (or
`build/libppb.so`) and include `include/ppb/ppb.h`.  The library has
no external dependencies and requires C11 (`-std=c11`).

Concepts
--------

**`struct ppb_buf`**: a read-only byte slice
`{ const void *buf; size_t size; }`.  PPB never writes through the
`buf` pointer.  `ppb_lexn` updates the struct in place to advance
through the buffer; `ppb_prescan` consumes a `ppb_buf` by value.  Both
construct subslices for length-prefixed payloads (variable-length
values).

**`struct ppb_encoded_tag`**: a tag identity created with
`PPB_TAG(uint64_t field_number, enum ppb_wire_type)`.  Callers
assemble a `const struct ppb_encoded_tag[]` array sorted in ascending
order and pass it to `ppb_prescan` / `ppb_lexn`; PPB never writes
through this pointer.  Field number `-1` (i.e., `UINT64_MAX` cast to
the field-number argument) is a catch-all that matches any unknown tag
of a given wire type.

The `tags` array *must be in strictly ascending* order (by `.bits`);
duplicates are rejected by `ppb_validate_tags` with
`PPB_ERROR_UNSORTED_FIELD_ARR`, same as unsorted arrays.

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
and `ppb_lexn` function families run in constant space and Θ(n log m)
time, where n is the number of toplevel fields consumed and m is
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
    PPB_ERROR_SENTINEL_FIELD_ARR =
        -2,  /* tags[].bits includes < 8 (field number 0 is forbidden by the protobuf spec) */
    PPB_ERROR_TRUNCATED_DATA = -3,  /* message cut short at the end of the `ppb_buf` */
    PPB_ERROR_CORRUPT_VARINT = -4,  /* invalid varint encoding (overlong) */
    PPB_ERROR_CORRUPT_TAG = -5,  /* invalid tag encoding (zero, overlong, or unsupported wire type) */
    PPB_ERROR_LIMIT_EXCEEDED = -6,  /* consumed bytes exceeded hard limit */
};

/*
 * Traverses `buf` scanning all toplevel fields (up to `max_lexed_fields`).
 * Tags must be pre-validated with `ppb_validate_tags`; skipping validation
 * does not cause undefined behaviour but may produce incorrect results or
 * trigger assertion failures when PPB is built with assertions.
 *
 * Returns the number of bytes traversed (equals `buf.size` on success),
 * or a negative `ppb_error`.
 */
static inline ptrdiff_t
ppb_prescan(struct ppb_buf buf, size_t num_fields, const struct ppb_encoded_tag *__restrict tags,
    struct ppb_field *__restrict fields, size_t max_lexed_fields)
{
    return ppb_prescan_impl(buf, num_fields, tags, fields, max_lexed_fields, SIZE_MAX, PPB_OK);
}

/*
 * Like `ppb_prescan`, but stops at the first field boundary where bytes
 * consumed >= `limit`.  If consumed bytes exceed `limit`, returns
 * `PPB_ERROR_LIMIT_EXCEEDED`.  Tags must be pre-validated with
 * `ppb_validate_tags`; skipping validation does not cause undefined
 * behaviour but may produce incorrect results or trigger assertion
 * failures when PPB is built with assertions.
 */
static inline ptrdiff_t
ppb_prescan_with_hard_limit(struct ppb_buf buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields)
{
    return ppb_prescan_impl(buf, num_fields, tags, fields, max_lexed_fields, limit, PPB_ERROR_LIMIT_EXCEEDED);
}

/*
 * Like `ppb_prescan`, but stops at the first field boundary where bytes
 * consumed >= `limit`.  Consuming more than `limit` bytes is not an error.
 * Tags must be pre-validated with `ppb_validate_tags`; skipping validation
 * does not cause undefined behaviour but may produce incorrect results or
 * trigger assertion failures when PPB is built with assertions.
 */
static inline ptrdiff_t
ppb_prescan_with_soft_limit(struct ppb_buf buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields)
{
    return ppb_prescan_impl(buf, num_fields, tags, fields, max_lexed_fields, limit, PPB_OK);
}

/*
 * Consumes from `buf` up to `max_lexed_fields` toplevel fields in
 * strictly ascending order.  Returns the decoded field range and status.
 * Tags must be pre-validated with `ppb_validate_tags`; skipping validation
 * does not cause undefined behaviour but may produce incorrect results or
 * trigger assertion failures when PPB is built with assertions.
 */
static inline struct ppb_lexn_ret
ppb_lexn(struct ppb_buf *__restrict buf, size_t num_fields, const struct ppb_encoded_tag *__restrict tags,
    struct ppb_field *__restrict fields, size_t max_lexed_fields)
{
    return ppb_lexn_impl(buf, num_fields, tags, fields, max_lexed_fields, SIZE_MAX, PPB_OK);
}

/*
 * Like `ppb_lexn`, but stops at the first field boundary where bytes
 * consumed >= `limit`.  If consumed bytes exceed `limit`, the returned
 * `status` is `PPB_ERROR_LIMIT_EXCEEDED`.  Tags must be pre-validated with
 * `ppb_validate_tags`; skipping validation does not cause undefined behaviour
 * but may produce incorrect results or trigger assertion failures when PPB
 * is built with assertions.
 */
static inline struct ppb_lexn_ret
ppb_lexn_with_hard_limit(struct ppb_buf *__restrict buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields)
{
    return ppb_lexn_impl(buf, num_fields, tags, fields, max_lexed_fields, limit, PPB_ERROR_LIMIT_EXCEEDED);
}

/*
 * Like `ppb_lexn`, but stops at the first field boundary where bytes
 * consumed >= `limit`.  Consuming more than `limit` bytes is not an error.
 * Tags must be pre-validated with `ppb_validate_tags`; skipping validation
 * does not cause undefined behaviour but may produce incorrect results or
 * trigger assertion failures when PPB is built with assertions.
 */
static inline struct ppb_lexn_ret
ppb_lexn_with_soft_limit(struct ppb_buf *__restrict buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields)
{
    return ppb_lexn_impl(buf, num_fields, tags, fields, max_lexed_fields, limit, PPB_OK);
}

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
 *
 * `*error` is sticky: if `*error` is already non-zero on entry the
 * function still consumes input and returns the decoded varint, but
 * `*error` is left unchanged.  Most callers should ensure `error`
 * is zero-initialized on entry.
 */
uint64_t ppb_decode_varint(struct ppb_buf *__restrict buf, enum ppb_error *__restrict error);

/*
 * Validates a tag array: checks that all entries have `.bits > 7`
 * (field number 0 is forbidden in protobuf) and that the array is
 * strictly sorted ascending.
 *
 * N.B., the encoding sticks the wire type in the low bits of the tag,
 * so a list of encoded tags with strictly ascending, strict positive,
 * field numbers is valid.  When there are multiple `ppb_encoded_tag`s
 * with the same field number (e.g., for catch-alls with -1), the types
 * must follow the order in `enum ppb_wire_type`.
 *
 * Call once on any static tag array before passing it to `ppb_prescan`
 * or `ppb_lexn`.  Passing an unvalidated array to prescan or lexn does
 * not cause undefined behaviour but may produce incorrect results or
 * trigger assertion failures when PPB is built with assertions.
 *
 * Returns `PPB_OK` on success, or the first error found.  The empty
 * tag array (`num_fields == 0`, `tags == NULL` allowed) returns
 * `PPB_OK`; the matching prescan / lexn calls then accept any
 * well-formed message and decode no fields, useful for
 * validate-only call sites.
 */
enum ppb_error ppb_validate_tags(size_t num_fields, const struct ppb_encoded_tag *tags);
```

Usage pattern
-------------

```c
#include "ppb/ppb.h"

enum { F_NAME = 0, F_ID = 1, F_DATA = 2, NUM_FIELDS };

/*
 * Tags are sorted ascending by .bits and never modified after init.
 * Call ppb_validate_tags once at program start (in main() or an
 * __attribute__((constructor)) function) to catch mis-sorted arrays
 * early: a bad tag array silently results in wrong output, not an error
 * (or maybe an assertion error if you built with assertions and get lucky).
 */
static const struct ppb_encoded_tag tags[NUM_FIELDS] = {
    [F_NAME] = PPB_TAG(1, PPB_WIRE_LEN),    /* string name = 1 */
    [F_ID]   = PPB_TAG(2, PPB_WIRE_VARINT), /* uint64 id   = 2 */
    [F_DATA] = PPB_TAG(5, PPB_WIRE_LEN),    /* bytes  data = 5 */
};
/* PPB_TAG preserves ordering by field and type: PPB_TAG(1,2) < PPB_TAG(2,0) < PPB_TAG(5,2). */

struct ppb_field fields[NUM_FIELDS] = { 0 };  /* mutable per-call state */

/* 0. Validate tags once at startup; bad arrays give wrong output, not errors. */
if (ppb_validate_tags(NUM_FIELDS, tags) != PPB_OK) abort();

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

    size_t end = r.first_field + r.field_range;
    /*
     * If field_range == UINT32_MAX, the width is unknown; extend
     * to NUM_FIELDS.  This can only happen when NUM_FIELDS >=
     * UINT32_MAX, a pretty niche use case.
     */
    if (r.field_range == UINT32_MAX) end = NUM_FIELDS;
    for (size_t i = r.first_field; i < end; i++)
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

**Detecting decoded fields**: in a `ppb_lexn` loop, save `buf.buf`
before the call as `old_buf`; afterwards, `old_buf <= field.v.ptr <
buf.buf` iff that field was decoded in this call.  Why "iff": both
`ppb_prescan` and `ppb_lexn` only write `ptr` when they match a tag,
and `ppb_lexn` consumes input monotonically, so a `ptr` written by
an earlier call lies strictly below `old_buf` and any `ptr` in
`[old_buf, buf.buf)` was set in this call.

For a one-shot `ppb_prescan` on a freshly zero-initialized `fields[]`,
the simpler `field.v.ptr != NULL` suffices.  `field.v.ptr` being
`NULL` means we never decoded the field, or attempted to and failed.
There's no way to tell the difference, so it's best to immediately
bail when `ppb_prescan*` or `ppb_lexn*` return an error.

**Nested submessages**: when a `PPB_WIRE_LEN` field is a submessage,
pass `field.v.payload` as the `buf` argument to a fresh `ppb_prescan`
/ `ppb_lexn` pair.  PPB is iterative within one message level;
recursion across levels is the caller's responsibility.  Ideally, the
nesting depth is capped in the schema itself.  When that's impossible,
remember to impose an arbitrary limit: unbounded recursion through
nested messages is a recurring issue in the protobuf ecosystem
([CVE-2024-7254](https://github.com/advisories/GHSA-735f-pc8j-v9w8),
[CVE-2026-0994](https://github.com/advisories/GHSA-7gcm-g887-7qv7)).

```c
int decode_msg(struct ppb_buf buf, size_t max_depth)
{
    if (max_depth == 0)
        return -1;

    /* ... prescan / lexn loop ... */
    {
        /* for each PPB_WIRE_LEN field that is a submessage: */
        if (decode_msg(fields[F_SUB].v.payload, max_depth - 1) < 0)
            return -1;
    }

    return 0;
}
```

**Repeated fields**: `ppb_lexn` stops as soon as a tag is
non-monotonic (repeated or lower than the previous tag), so each call
yields at most one occurrence of any given field.  `field.v` holds the
value decoded in this call; the outer loop naturally delivers each
occurrence in the encoded order.

**Catch-all entries**: use `PPB_TAG(-1, wire_type)` to match any field
of a given wire type that wasn't matched by a specific entry.
Catch-all entries sort after all specific entries.  `ppb_lexn` always
stops after a catch-all match, so place catch-alls only where you can
afford a potential overhead of one `ppb_lexn` call per field.

PPB matches tags by their full encoded form, so a wire-type mismatch
on a known field number (e.g., the schema declares field 5 as
`PPB_WIRE_VARINT` but the wire encoding is for a `fixed64`) is
silently treated as an unknown field: the tag misses the specific
entry, and is either routed to a catch-all of the wire type actually
on the wire or skipped.  Callers that need strict wire-type checking
must enforce it themselves, probably through catch-alls.

**Value fields** in `struct ppb_field_value`:
- `u64` / `i64`: full 64 bits, host byte order (the assembled value:
  a decoded varint, the bits of a fixed64, or a zero-extended fixed32).
- `u32` / `i32` / `f`: low 32-bit views (for fixed32 fields).
- `d`: double view of the full 64 bits.
- `b[8]`: raw bytes of `u64` in host order — *not* the wire encoding.
- `payload`: `struct ppb_buf` pointing at the payload of a
  `PPB_WIRE_LEN` field.
- `ptr`: pointer to the field's tag byte in the original buffer
  (useful for catch-all).

The union works on either endian: on big-endian hosts the 32-bit
views are laid out so they alias the low half of `u64`.

**Zigzag**: call `ppb_zag(field.v.u64)` to decode `sint32` / `sint64` values.

**Byte length limiting**: use `ppb_prescan_with_hard_limit` /
`ppb_lexn_with_hard_limit` when the message length is known to be
smaller than the remaining readable region: the hard limit avoids the
end-of-buffer slow paths around the end of the message.  Use the
`_with_soft_limit` variants to implement chunked processing at
approximate boundaries: pass a target chunk size as the limit and PPB
will stop at the first clean field boundary at or past that size, so
the chunk always ends just before another valid field.  The soft limit
can be exceeded by up to one field's worth: with a soft limit, the
final field of the chunk is always parsed in full instead of returning
a spurious error.

```c
/* Chunked processing: stop at every ~CHUNK_SIZE bytes. */
struct ppb_buf cur = msg;
while (cur.size > 0)
{
    /* Zero `fields[]` so each chunk's stats stand alone. */
    memset(fields, 0, sizeof(fields));
    ptrdiff_t scanned = ppb_prescan_with_soft_limit(
        cur, /*limit=*/CHUNK_SIZE, NUM_FIELDS, tags, fields, SIZE_MAX);
    if (scanned < 0) { /* error */ break; }

    process_chunk(fields, cur, scanned);  /* copy `fields` if spawning threads */
    cur.buf = (const char *)cur.buf + scanned;
    cur.size -= (size_t)scanned;
}
```

**Recovery after error**: after a negative `ppb_error` from `ppb_lexn`
or `ppb_prescan`, the message is unrecoverable.  PPB does not support
resuming a partial parse, and `ppb_buf` is not a streaming abstraction.
On a `ppb_lexn` error, `*buf` points somewhere within the offending
field (at its first tag or payload byte; the exact position is an
implementation detail), but always after all successfully decoded
fields.  The contents of the `fields[]` array are unspecified.  The
array is safe to access, but we make no other guarantee.

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

    make                # build/libppb.a, build/libppb.so, build/picoscope, build/ubench
    make test           # run unit tests + golden tests
    make unit           # unit tests only (build/test_ppb)
    make test EXTRA_FLAGS='-fsanitize=undefined,address'  # same with ubsan & asan
    make fuzz           # quick libfuzzer-based run (requires clang)
    make wp             # Frama-C WP verification (requires Docker)
    make eva            # Frama-C EVA + WP (requires Docker)
    make format         # clang-format-20 on all sources
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
