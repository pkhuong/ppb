#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * PPB (Pico Protobuf) is an allocation-free lexer for modern (v2 or
 * v3, without groups) protobuf-encoded bytes.
 *
 * An initial `ppb_prescan` validates the input message and gathers
 * statistics about all the toplevel fields in the message.  With the
 * number of occurrences for each field, as well as the total, min and
 * max number of payload bytes for length-prefixed fields (bytes,
 * strings, nested messages, or packed repeated fields), a caller can
 * easily pre-allocate storage for a message before `ppb_lexn`.
 *
 * The runtime complexity of `ppb_prescan` is `\Theta(m + n log m)`,
 * where `n` is the number of toplevel fields and `m` the number of
 * `ppb_fields`, regardless of the actual size of the encoded bytes;
 * that's what makes it reasonble to "lex" toplevel fields only, and
 * recursively handle nested messages separately.
 *
 * The `ppb_lexn` function gives the caller access to every single
 * field in the message... but, in the common case of a message with
 * at most once occurrence of every field, and fields emitted in
 * strictly ascending order, `ppb_lexn` does that in a single call:
 * `ppb_lexn` returns a range of indices in an array of `ppb_field`s,
 * where the indices bracket all fields that were decoded and matched
 * to a `ppb_field`.  That range is encoded as the index in that array
 * of the first lexed (and not skipped) field, and the width of that
 * range (or UINT32_MAX if unknown).
 *
 * The runtime complexity of `ppb_lexn` is also `\Theta(n log m)`, where
 * `n` is the number of toplevel fields decoded in the call and `m` the
 * number of `ppb_field`s.
 *
 * The complexity of both `ppb_prescan` and `ppb_lexn`, linear in the number
 * of *toplevel fields* but otherwise independent of the input buffer size,
 * is key to the simplistic zero-copy lexing approach: it's reasonable for the
 * caller to recursively lex nested submessages.  The prescan before lexing
 * structure was designed for arena allocation: the statistics should suffice
 * to preallocate the current message, including its variable-length members,
 * before lexing the message and recursively decoding submessages in the arena.
 */

/*
 * All PPB public functions use the same error enum.  Error codes are
 * always strictly negative.
 */
enum ppb_error
{
    PPB_OK = 0,
    PPB_ERROR_UNSORTED_FIELD_ARR = -1,  /* tags[].bits not strictly ascending */
    PPB_ERROR_SENTINEL_FIELD_ARR = -2,  /* tags[].bits includes < 8 (index 0 is invalid in protobuf) */
    PPB_ERROR_TRUNCATED_DATA = -3,  /* message cut short at the end of the `ppb_buf` */
    PPB_ERROR_CORRUPT_VARINT = -4,  /* invalid varint encoding (overlong) */
    PPB_ERROR_CORRUPT_TAG = -5,  /* invalid tag encoding (zero, overlong, or unsupported wire type) */
    PPB_ERROR_LIMIT_EXCEEDED = -6,  /* consumed bytes exceeded hard limit */
};

/*
 * PPB never takes ownership of storage, but we have to pass slices of
 * encoded bytes.  `ppb_buf` is that slice type; PPB never writes
 * through such slices.
 */
struct ppb_buf
{
    const void *buf;
    size_t size; /* in bytes */
};

/*
 * Protobuf wire encoding types.
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
 */
#define PPB_ENCODE_VARINT(VARINT)                                                                \
    (PPB_VB_((VARINT), 0) | PPB_VB_((VARINT), 1) | PPB_VB_((VARINT), 2) | PPB_VB_((VARINT), 3) | \
        PPB_VB_((VARINT), 4) | PPB_VB_((VARINT), 5) | PPB_VB_((VARINT), 6) | PPB_VB_((VARINT), 7))

#define PPB_VB_(VARINT, SHIFT)                                                                       \
    ((uint64_t)((((VARINT) >> ((SHIFT) * 7)) & 0x7F) | (((VARINT) >> ((SHIFT) * 7 + 7)) ? 0x80 : 0)) \
        << ((SHIFT) * 8))

/*
 * Encodes a (field_number, wire_type) pair into the encoded_tag
 * format expected by ppb_field.  For positive field numbers, the
 * result is the varint-encoded tag in little endian (matching the
 * internal representation used by ppb_lexn).
 *
 * Field number 0 is forbidden by the protobuf spec.
 * Field number -1 (UINT64_MAX when unsigned) is reserved for
 * catch-all entries: a catch-all field matches any unknown tag
 * of the given wire type.
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
 * field id, and, for length-prefixed values, the total/min/max number
 * of bytes for the payload.
 */
struct ppb_field_meta
{
    size_t num_occurrences;
    size_t total_bytes;
    size_t min_nonzero_bytes;
    size_t max_bytes;
};

/*
 * For each field, we track the latest value we've decoded, if any.
 *
 * When PPB doesn't encounter the field, its value is left untouched;
 * usually, that means 0-initialized, or at its previous value.
 *
 * This means we can save the `ppb_buf::buf` pointer and compare it
 * with `ptr`: a field value has `ptr >= old_buf` iff it was decoded
 * in the most recent call to `ppb_prescan` / `ppb_lexn`.
 */
struct ppb_field_value
{
    /* Pointer to the field's tag, populated only by `ppb_lexn`. */
    const void *ptr;
    /* Varint or i64/f64/i32/f32 field */

    /*
     * Contents as little-endian u64, or raw bytes of fixed 64
     * encoding.
     */
    union
    {
        uint64_t u64;
#ifndef __FRAMAC__
        uint8_t b[8];
        double d;
        /* XXX: assumes little endian here. */
        uint32_t u32;
        float f;
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
 *
 * Call once on any static tag array before passing it to `ppb_prescan` or
 * `ppb_lexn`.  Passing an unvalidated array to prescan or lexn does not cause
 * undefined behaviour, but may produce incorrect results.
 *
 * Returns `PPB_OK` on success, or the first error found.
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
 * toplevel fields, stopping early when the number of bytes consumed
 * reaches or exceeds `limit`.  Stopping before or exactly at `limit`
 * is always OK; stopping past `limit` signals `limit_error` if it is
 * not `PPB_OK` (hard limit), and is silent otherwise (soft limit).
 * Pass `limit = SIZE_MAX` and `limit_error = PPB_OK` for the
 * unlimited behavior of `ppb_lexn`.
 *
 * This function may decode multiple fields at a time, but only in
 * strictly ascending order, and always stops before a non-monotonic
 * (repeated or decreasing) tag or after an unknown field.  This
 * guarantees that we can always recover the order of the fields on
 * the wire, and that we always see every field value.  Call
 * `ppb_lexn*` in a loop to process all fields in a message, in
 * batches of strictly monotonically increasing fields.
 *
 * Updates `fields[].v` in place, and sets `ptr` to the decoded
 * field's first byte in `buf` (i.e., where the tag varint begins);
 * check if we have just decoded the field by comparing its `ptr` with
 * the initial value of `buf.buf`.
 *
 * Assumes the tags are sorted and valid (no zero tag);
 * ppb_prescan_impl checks for that.
 */
struct ppb_lexn_ret ppb_lexn_impl(struct ppb_buf *__restrict buf, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields, size_t limit, enum ppb_error limit_error);

#ifdef __cplusplus
}  // extern "C"
#endif

/*
 * Traverses `buf` scanning all toplevel fields (up to `max_lexed_fields`).
 * Tags must be pre-validated with `ppb_validate_tags`; passing invalid tags
 * may cause incorrect decoding.
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
 * `PPB_ERROR_LIMIT_EXCEEDED`.  Tags must be pre-validated.
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
 * Tags must be pre-validated with `ppb_validate_tags`; skipping
 * validation does not cause undefined behaviour but may produce
 * incorrect results.
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
 * does not cause undefined behaviour but may produce incorrect results.
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
 * but may produce incorrect results.
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
 * does not cause undefined behaviour but may produce incorrect results.
 */
static inline struct ppb_lexn_ret
ppb_lexn_with_soft_limit(struct ppb_buf *__restrict buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields)
{
    return ppb_lexn_impl(buf, num_fields, tags, fields, max_lexed_fields, limit, PPB_OK);
}
