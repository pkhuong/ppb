#pragma once
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/*
 * PPB (Pico Protobuf) is an allocation-free lexer for modern (v2 or
 * v3, without groups) protobuf-encoded bytes.  It is designed to
 * parse contiguous buffers of untrusted bytes, with fuzz testing and
 * formal verification for lack of undefined behavior, memory unsafety
 * or logic bugs.
 *
 * PPB only consumes protobuf bytes; it does not produce them.  The
 * wire format is well specified, so it's fine to use Google's
 * official protobuf libraries for encoding (or decoding, when you
 * don't need PPB's guarantees).  When the producer side also cares
 * about allocations and copies, ProtoZero
 * (https://perfetto.dev/docs/design-docs/protozero,
 * https://github.com/google/perfetto/tree/main/include/perfetto/protozero)
 * pairs well with PPB.
 *
 * Public API surface, in expected order of use:
 *   - `ppb_validate_tags`: call once on each static tag array; the
 *     remaining functions assume tags are valid.
 *   - `ppb_prescan` (and `_with_hard_limit` / `_with_soft_limit`):
 *     validate the message and accumulate per-field metadata.
 *   - `ppb_lexn` (and `_with_hard_limit` / `_with_soft_limit`): walk
 *     the message in batches of strictly monotonically-increasing
 *     known fields; call in a loop.
 *   - `PPB_TAG(-1, wire_type)`: catch-all entry that matches any
 *     field of that wire type not listed individually.
 *   - Helpers: `ppb_zag` (zigzag), `ppb_decode_varint`.
 *
 * Decoding quirks (more in README.md, under "Gotchas, decoding quirks, and footguns"):
 *   - Varints may span up to 10 bytes, and may contain redundant
 *     bytes; for the 10th byte (which must have its top bit unset),
 *     only the bottom bit contributes to the result, and bits [6:1]
 *     are silently discarded.
 *   - Wire types 3, 4 (legacy groups) and 6, 7 (reserved) are rejected
 *     with `PPB_ERROR_CORRUPT_TAG`.
 *   - Fields are matched on both field number and encoding type; an
 *     encoded field that matches only on the field number is treated
 *     like an unknown field (skipped or passed to catch-all).
 *   - Encoded fields are matched exclusively in their canonical
 *     (minimal) varint encoding.
 *   - There is no check that string values are utf-8 encoded: on the
 *     wire, string, bytes, submessages and packed repeated fields are
 *     all the same.
 *
 * An initial `ppb_prescan` validates the input message and gathers
 * statistics about all the toplevel fields in the message.  With the
 * number of occurrences for each field, as well as the total, min and
 * max number of payload bytes for length-prefixed fields (bytes,
 * strings, nested messages, or packed repeated fields), a caller can
 * easily pre-allocate storage for a message before `ppb_lexn`.
 *
 * The library is trivially thread-safe and reentrant, since it
 * doesn't have any global state.  It should be safe to call the
 * library on any `ppb_buf` inputs; the other (trusted) inputs may
 * also be invalid (e.g., an unsorted tag array or non-zero-initialized
 * `fields[]`) and at worst result in surprising results or, in builds
 * with assertions enabled, assertion failures; never memory
 * unsafety, undefined behavior, non-termination, or successful return
 * without progress (for `lexn`).  Frama-C's WP discharges memory
 * safety, termination, and progress (`ppb_lexn` always consumes at
 * least one byte or errors) regardless of the contents of input
 * buffers and arrays (trusted or otherwise); see `SECURITY.md` for
 * the full two-tier breakdown of trusted-input preconditions.
 *
 * Both `ppb_prescan` and `ppb_lexn` run in `\Theta(n log m)` time,
 * where `n` is the number of toplevel fields consumed and `m` the
 * number of `ppb_field`s, independent of the encoded payload size.
 * That linearity in the number of *toplevel fields* makes recursive
 * descent on nested submessages practical, and motivates the
 * prescan-before-lexing structure: the statistics should suffice to
 * preallocate the current message, including its variable-length
 * members, in an arena before lexing the message and recursively
 * decoding submessages.
 *
 * The `ppb_lexn` function gives the caller access to every single
 * field in the message... but, in the common case of a message that
 * contains only known fields emitted in strictly ascending order with
 * at most one occurrence each, `ppb_lexn` does that in a single call:
 * `ppb_lexn` returns a range of indices in an array of `ppb_field`s,
 * where the indices bracket all fields that were decoded and matched
 * to a `ppb_field`.  That range is encoded as the index in that array
 * of the first lexed (and not skipped) field, and the width of that
 * range (or UINT32_MAX if unknown).
 */

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
    PPB_ERROR_CORRUPT_VARINT = -4,  /* invalid varint encoding (longer than 10 bytes) */
    /*
     * invalid tag encoding (canonical zero, longer than 8 bytes, or unsupported wire type).
     *
     * In addition to these structural issues, prescan *but not lexn* also flags
     * non-canonical (overlong) encodings of tags with field index 0.
     */
    PPB_ERROR_CORRUPT_TAG = -5,
    PPB_ERROR_LIMIT_EXCEEDED = -6,  /* consumed bytes exceeded hard limit */
    PPB_ERROR_DEPTH_EXCEEDED = -7,  /* recursion depth budget exhausted (for ppb.hpp and client code) */

    /*
     * This error code is reserved for the convenience of client code
     * and never actually generated by PPB (C core or C++ wrapper).
     */
    PPB_ERROR_INVALID_UTF8 = -64  /* string field is not well-formed UTF-8 (not checked by PPB) */
};

/*
 * PPB never takes ownership of storage, but we have to pass slices of
 * encoded bytes.  `ppb_buf` is that slice type; PPB never writes
 * through such slices.  The contents of `ppb_buf` are untrusted and
 * validated as needed.
 *
 * A valid slice may not have more than `PTRDIFF_MAX` bytes (otherwise,
 * it's very easy to introduce undefined behavior in programs, c.f.
 * https://www.trust-in-soft.com/resources/blogs/2016-05-20-objects-larger-than-ptrdiff_max-bytes).
 */
struct ppb_buf
{
    const void *buf;
    size_t size; /* in bytes, at most PTRDIFF_MAX */
};

/*
 * Protobuf wire encoding types.
 *
 * Wire types 3 (legacy SGROUP) and 4 (legacy EGROUP) are not
 * supported; the parser returns `PPB_ERROR_CORRUPT_TAG` on either.
 * Wire types 6 and 7 are reserved by the protobuf spec and also
 * rejected.
 */
enum ppb_wire_type
{
    PPB_WIRE_VARINT = 0,  /* Value is a varint */
    PPB_WIRE_I64 = 1,  /* value is a fixed 64-bit field */
    PPB_WIRE_LEN = 2,  /* value is a length-prefix record (message, bytes, packed) */
    PPB_WIRE_I32 = 5,  /* value is a fixed 32-bit field */
};

/*
 * Encodes a varint in little-endian byte order, in a uint64_t.
 * `VARINT` must not have side effects.
 *
 * This macro is only meant for tags, and truncates values >= 2**56
 * (that don't fit in 64 bit once encoded as varint).
 */
#define PPB_ENCODE_VARINT(VARINT)                                                                          \
    (PPB_VB_((uint64_t)(VARINT), 0) | PPB_VB_((uint64_t)(VARINT), 1) | PPB_VB_((uint64_t)(VARINT), 2) |    \
        PPB_VB_((uint64_t)(VARINT), 3) | PPB_VB_((uint64_t)(VARINT), 4) | PPB_VB_((uint64_t)(VARINT), 5) | \
        PPB_VB_((uint64_t)(VARINT), 6) | PPB_VB_((uint64_t)(VARINT), 7))

#define PPB_VB_(VARINT, SHIFT)                                                                       \
    ((uint64_t)((((VARINT) >> ((SHIFT) * 7)) & 0x7F) | (((VARINT) >> ((SHIFT) * 7 + 7)) ? 0x80 : 0)) \
        << ((SHIFT) * 8))

/*
 * Encodes a (field_number, wire_type) pair into the encoded_tag
 * format expected by ppb_field.  Both `FIELD_NUMBER` and `WIRE_TYPE`
 * must be safe for multiple evaluation.
 *
 * Field number 0 is forbidden by the protobuf spec.
 * Field number -1 (UINT64_MAX when unsigned) is reserved for
 * catch-all entries: a catch-all field matches any unknown tag
 * of the given wire type.
 *
 * N.B., only -1 is reserved for catch-all; other negative field
 * numbers are silently encoded as huge unsigned values.
 */
#define PPB_TAG_BITS(FIELD_NUMBER, WIRE_TYPE)             \
    ((uint64_t)(FIELD_NUMBER) == (uint64_t)-1 ?           \
            ((UINT64_MAX << 3) | (uint64_t)(WIRE_TYPE)) : \
            PPB_ENCODE_VARINT(((uint64_t)(FIELD_NUMBER) << 3) | (uint64_t)(WIRE_TYPE)))

#define PPB_TAG(FIELD_NUMBER, WIRE_TYPE) { .bits = PPB_TAG_BITS(FIELD_NUMBER, WIRE_TYPE) }

/*
 * PPB works with pre-varint encoded tags (no need to fully decode
 * when we can just compare after masking).
 */
struct ppb_encoded_tag
{
    uint64_t bits;
};

/*
 * For each field, we always count the total number of hits for the
 * field id, and the total/min/max number of value bytes.
 *
 * `total_bytes`, `min_nonzero_bytes`, and `max_bytes` are tracked for
 * every wire type: for VARINT, that's the varint's encoding length in
 * bytes for each occurrence; for I32/I64, each occurrence contributes
 * 4 or 8 bytes respectively; for LEN, the length-prefixed payload size.
 *
 * Stealing one bit from `num_occurrences` is safe from overflow since
 * any encoded occurrence needs at least two bytes on the wire (one
 * tag byte + one payload byte), and the maximum buffer is
 * PTRDIFF_MAX, half of SIZE_MAX on the platforms we care about.
 * That's room for *two* stolen bits; we only take one (and
 * static_assert that we can in ppb.c).
 *
 * The `lost_distinct_u64` flag tells us whether `ppb_prescan` lost a
 * distinct prior `v.u64` to last-write-wins.  Since each
 * `ppb_encoded_tag` specifies the wire type, the flag's meaning is
 * unambiguous for each field:
 *
 *   - VARINT: two or more occurrences had distinct decoded values
 *   - I64 / I32: two or more occurrences had distinct bit patterns
 *   - LEN: flag is never set
 *
 * Rarely useful for catch-all entries (`PPB_TAG(-1, ...)`): a
 * catch-all aggregates distinct field numbers, so the flag may fire
 * even when no individual field number repeats.
 */
struct ppb_field_meta
{
    size_t lost_distinct_u64 : 1;
    size_t num_occurrences : CHAR_BIT * sizeof(size_t) - 1;
    size_t total_bytes;

    /*
     * `min_nonzero_bytes == 0` means either the field never occurred
     * or every occurrence was zero-length (same for `max_bytes`);
     * check `num_occurrences` to disambiguate.
     */
    size_t min_nonzero_bytes;
    size_t max_bytes;
};

/*
 * For each field, we track the latest value we've decoded, if any.
 *
 * When PPB doesn't encounter the field, its value is left untouched;
 * usually, that means 0-initialized, or at its previous value.
 *
 * To detect whether a field was decoded:
 *   - After a one-shot `ppb_prescan` on a freshly zero-initialized
 *     `fields[]`, `field.v.ptr != NULL` iff the field was matched.
 *   - In a `ppb_lexn` loop, save `buf.buf` as `old_buf` before the
 *     call; afterwards, the field was decoded in this call iff
 *         old_buf <= field.v.ptr < buf.buf
 *     (the call advances `buf.buf` past everything it consumed).
 *
 * **DO NOT USE `ptr != NULL` with `ppb_lexn`: the `ptr` is sticky!**
 */
struct ppb_field_value
{
    /*
     * Pointer to the field's tag byte in the input buffer.  Both
     * `ppb_prescan` and `ppb_lexn` set `ptr` after a successful
     * match (retaining the *last* occurrence per call for repeated
     * fields) and leave `ptr == NULL` otherwise.  A `NULL` ptr can
     * mean the field was never encountered (still zero-initialized),
     * but it could also be zeroed after a decoding failure.
     */
    const void *ptr;

    /*
     * Decoded value for numeric fields.
     *
     * `u64` holds the value in native host order: a decoded varint,
     * the raw bits of a fixed64 reinterpreted as a uint64, or the
     * zero-extended bits of a fixed32.  `b[]`, `d`, `i64`, `u32`,
     * `i32`, and `f` are typed views of the same bits; the
     * big-endian block below places `u32`/`i32`/`f` at an offset
     * so they always alias the low half of `u64` regardless of
     * host endianness.
     *
     * N.B., `b[]` is the host-byte-order layout of the assembled
     * `u64`, not its protobuf wire encoding.
     *
     * The non-`u64` members are hidden from Frama-C: they would
     * force WP to reason about union punning, which is not always
     * dischargeable.  Library users compiling normal builds are
     * unaffected, and PPB itself only accesses `u64`.
     */
    union
    {
        uint64_t u64;
#ifndef __FRAMAC__
        uint8_t b[8];
        int64_t i64;
        double d;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        uint32_t u32;
        int32_t i32;
        float f;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        struct
        {
            uint32_t ppb_be_padding_do_not_use;
            union
            {
                uint32_t u32;
                int32_t i32;
                float f;
            };
        };
#else
    /* we can still access 8-byte fields regardless of host endianness. */
#warning "unknown byte order (neither little nor big)"
#endif
#endif
    };

    /*
     * Variable-length payload (points at the payload, after the
     * length header).
     */
    struct ppb_buf payload;
};

struct ppb_field
{
    struct ppb_field_meta m;
    struct ppb_field_value v;
};

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Decodes zigzag-encoded sint32 and sint64 values.
 *
 * A 32-bit unsigned int value will decode to an `int32_t`,
 * but you want to use `ppb_zag32` for sint32.
 */
static inline int64_t
ppb_zag(uint64_t x)
{
    return (int64_t)((x >> 1) ^ -(x & 1));
}

/*
 * Decodes zigzag-encoded sint32 values.
 *
 * This is the same thing as `ppb_zag`, except the encoded value is
 * truncated to 32 bits before decoding, in order to match google's
 * C++ implementation (only relevant with non-canonical encoding).
 */
static inline int32_t
ppb_zag32(uint32_t x)
{
    return (int32_t)ppb_zag(x);
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
 * not cause undefined behavior but may produce incorrect results or
 * trigger assertion failures when PPB is built with assertions.
 *
 * Returns `PPB_OK` on success, or the first error found.  The empty
 * tag array (`num_fields == 0`, `tags == NULL` allowed) returns
 * `PPB_OK`; the matching prescan / lexn calls then accept any
 * well-formed message and decode no fields, useful for
 * validate-only call sites.
 */
enum ppb_error ppb_validate_tags(size_t num_fields, const struct ppb_encoded_tag *tags);

/*
 * Traverses `buf` until it's lexed up to `max_lexed_fields` toplevel
 * fields, stopping early when the number of bytes consumed reaches or
 * exceeds `limit`.  Stopping before or exactly at `limit` is always
 * OK; stopping past `limit` (because a field straddled the boundary)
 * signals `limit_error` if `limit_error != PPB_OK` (hard limit), and
 * is silent otherwise (soft limit).  Pass `limit = SIZE_MAX` and
 * `limit_error = PPB_OK` for the unlimited behavior of `ppb_prescan`.
 *
 * Returns the number of bytes traversed, or a negative `ppb_error`.
 *
 * `tags` is a parallel array of `num_fields` encoded tags (sorted
 * ascending) that identifies which fields to decode; `fields` is the
 * mutable output array.  Both must have at least `num_fields` entries
 * and must not overlap.
 *
 * Updates `fields[].m` with aggregate metadata for all the fields
 * seen, and populates `fields[].v` with the last value seen for each
 * field, if any.
 *
 * Tags must be pre-validated with `ppb_validate_tags`; skipping
 * validation does not cause undefined behavior but may produce
 * incorrect results or trigger assertion failures when PPB is built
 * with assertions.
 *
 * `max_lexed_fields` caps the number of toplevel decoding iterations,
 * including unknown fields that get skipped (so a `max_lexed_fields`
 * of 10 may decode only 1 known field if 9 unknowns appear first).
 * Passing `max_lexed_fields == 0` is a valid no-op.
 */
ptrdiff_t ppb_prescan_impl(struct ppb_buf buf, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields, size_t limit, enum ppb_error limit_error);

/*
 * A `ppb_lexn_ret` describes a range of indices in `fields` that
 * contain all fields decoded in that call to `ppb_lexn` (but may
 * include indices for irrelevant fields), and an error code or
 * `PPB_OK`.
 *
 * The range is `fields[first_field .. first_field + field_range)`,
 * with `field_range <= num_fields` and `first_field + field_range <=
 * num_fields`.  `field_range` saturates at `UINT32_MAX`: when
 * `field_range == UINT32_MAX`, the actual width is unknown, so treat
 * the range as `fields[first_field .. num_fields)`.  Saturation is
 * unreachable for any reasonable `num_fields <= UINT32_MAX`, including
 * every valid protobuf schema (field numbers fit in 29 bits).
 *
 * The reported range may include indices the call did not match
 * (only one contiguous range is reported, even when the call decoded
 * a sparse subset).  Always filter with `field.v.ptr` to recover the
 * indices actually touched in the current call.
 */
struct ppb_lexn_ret
{
    size_t first_field;
    uint32_t field_range;  /* saturates at UINT32_MAX */
    enum ppb_error status;
};

/*
 * Consumes from `buf` until it's lexed up to `max_lexed_fields`
 * toplevel fields, stopping early when the number of bytes consumed
 * reaches or exceeds `limit`.  Stopping before or exactly at `limit`
 * is always OK; stopping past `limit` signals `limit_error` if it is
 * not `PPB_OK` (hard limit), and is silent otherwise (soft limit).
 * Pass `limit = SIZE_MAX` and `limit_error = PPB_OK` for the
 * unlimited behavior of `ppb_lexn`.
 *
 * This function may decode multiple fields at a time, but only in
 * strictly ascending order: it stops before any tag that is not
 * strictly greater than the last *matched* tag, and after a
 * catch-all match.  Each call thus decodes at most one occurrence of
 * any matched field, and the wire order of matched fields is always
 * recoverable.  Skipped unknown fields neither end the batch nor
 * take part in the ordering check; install catch-alls to make every
 * field participate.  Call `ppb_lexn*` in a loop to process all
 * fields in a message, in batches of strictly monotonically
 * increasing matched fields.
 *
 * Updates `fields[].v` in place, and sets `ptr` to the decoded
 * field's first byte in `buf` (i.e., where the tag varint begins);
 * check if we have just decoded the field by comparing its `ptr` with
 * the initial value of `buf.buf`.
 *
 * On error, `buf` points somewhere within the offending field (at its
 * first tag or payload byte; the exact position is an implementation
 * detail), but always after all successfully decoded fields.
 *
 * Tags must be pre-validated with `ppb_validate_tags`; skipping
 * validation does not cause undefined behavior but may produce
 * incorrect results or trigger assertion failures when PPB is built
 * with assertions.
 *
 * `max_lexed_fields` caps the number of toplevel decoding iterations,
 * including unknown fields that get skipped (so a `max_lexed_fields`
 * of 10 may decode only 1 known field if 9 unknowns appear first).
 * Passing `max_lexed_fields == 0` is a valid no-op: `*buf` and
 * `fields[]` are left unchanged.
 */
struct ppb_lexn_ret ppb_lexn_impl(struct ppb_buf *__restrict buf, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields, size_t limit, enum ppb_error limit_error);

/*
 * Traverses `buf` scanning all toplevel fields (up to `max_lexed_fields`).
 * Tags must be pre-validated with `ppb_validate_tags`; skipping validation
 * does not cause undefined behavior but may produce incorrect results or
 * trigger assertion failures when PPB is built with assertions.
 *
 * Returns the number of bytes traversed, or a negative `ppb_error`.
 * The count equals `buf.size` when the whole buffer was scanned; a
 * smaller count means `max_lexed_fields` stopped the scan early.
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
 * behavior but may produce incorrect results or trigger assertion
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
 * does not cause undefined behavior but may produce incorrect results or
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
 * does not cause undefined behavior but may produce incorrect results or
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
 * `ppb_validate_tags`; skipping validation does not cause undefined behavior
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
 * does not cause undefined behavior but may produce incorrect results or
 * trigger assertion failures when PPB is built with assertions.
 */
static inline struct ppb_lexn_ret
ppb_lexn_with_soft_limit(struct ppb_buf *__restrict buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields)
{
    return ppb_lexn_impl(buf, num_fields, tags, fields, max_lexed_fields, limit, PPB_OK);
}

#ifdef __cplusplus
}  // extern "C"
#endif
