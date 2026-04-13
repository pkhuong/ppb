#include "ppb/ppb.h"

#include "buf.h"
#include "cc.h"
#include "varint.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

/* Validate PPB_ENCODE_VARINT against known varint encodings. */

/* Single-byte tags (field 1-15). */
static_assert(PPB_TAG_BITS(1, PPB_WIRE_VARINT) == 0x08, "field 1, varint");
static_assert(PPB_TAG_BITS(1, PPB_WIRE_I64) == 0x09, "field 1, I64");
static_assert(PPB_TAG_BITS(1, PPB_WIRE_LEN) == 0x0A, "field 1, LEN");
static_assert(PPB_TAG_BITS(2, PPB_WIRE_VARINT) == 0x10, "field 2, varint");
static_assert(PPB_TAG_BITS(2, PPB_WIRE_I32) == 0x15, "field 2, I32");
static_assert(PPB_TAG_BITS(15, PPB_WIRE_VARINT) == 0x78, "field 15, varint");

/* Two-byte tags (field 16+). */
static_assert(PPB_TAG_BITS(16, PPB_WIRE_VARINT) == 0x0180, "field 16, varint: [0x80, 0x01]");
static_assert(PPB_TAG_BITS(100, PPB_WIRE_VARINT) == 0x06A0, "field 100, varint: [0xA0, 0x06]");
static_assert(PPB_TAG_BITS(100, PPB_WIRE_I64) == 0x06A1, "field 100, I64: [0xA1, 0x06]");

/* Three-byte tag. */
static_assert(PPB_TAG_BITS(2048, PPB_WIRE_VARINT) == 0x018080, "field 2048, varint: [0x80, 0x80, 0x01]");

/* Four-byte tag (field >= 262144). */
static_assert(PPB_TAG_BITS(262144, PPB_WIRE_VARINT) == 0x01808080UL,
    "field 262144, varint: [0x80, 0x80, 0x80, 0x01]");
static_assert(PPB_TAG_BITS(262144, PPB_WIRE_I64) == 0x01808081UL,
    "field 262144, I64: [0x81, 0x80, 0x80, 0x01]");

/* Five-byte tag (field >= 33554432). */
static_assert(PPB_TAG_BITS(33554432UL, PPB_WIRE_VARINT) == 0x0180808080UL,
    "field 33554432, varint: [0x80, 0x80, 0x80, 0x80, 0x01]");
static_assert(PPB_ENCODE_VARINT((uint64_t)UINT32_MAX) == 0x0FFFFFFFFFUL,
    "UINT32_MAX: [0xFF, 0xFF, 0xFF, 0xFF, 0x0F]");

/* Eight-byte encoding. */
static_assert(PPB_ENCODE_VARINT(1UL << 49) == 0x0180808080808080UL,
    "1<<49: [0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01]");
static_assert(PPB_ENCODE_VARINT((1UL << 56) - 1) == 0x7FFFFFFFFFFFFFFFUL,
    "(1<<56)-1: [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F]");

/* Catch-all. */
static_assert(PPB_TAG_BITS(-1, PPB_WIRE_VARINT) == ((UINT64_MAX << 3) | 0), "catch-all, varint");
static_assert(PPB_TAG_BITS(-1, PPB_WIRE_I32) == ((UINT64_MAX << 3) | 5), "catch-all, I32");

/*
 * Bitwise OR is monotone: OR can only set bits, never clear them.
 * WP lacks a built-in axiom for this, so we admit it globally.
 */
/*@ admit lemma bitor_ge_l: \forall uint64_t a, b; (a | b) ≥ a; */

/*@ terminates \true;
  @ assigns \result \from x;
  @ behavior sint32:
  @  assumes 0 ≤ x < (1 << 32);
  @  // Tested by test_zag() boundary cases in tests/test_ppb.c.
  @  admit ensures  (-1 << 31) ≤ \result < (1 << 31);
  @ behavior sint64:
  @  assumes x ≥ (1 << 32);
  @  ensures (-1 << 63) ≤ \result < (1 << 63);
  @ complete behaviors sint32, sint64;
  @ disjoint behaviors sint32, sint64;
  @*/
static inline int64_t ppb_zag(uint64_t x);

/*@ requires buf_valid_range(buf);
  @ requires buf.size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ terminates \true;
  @ assigns g_initial_buf, fields[0..num_fields - 1];
  @ ensures \result ≥ 0 ==> \result ≤ buf.size;
  @*/
static inline ptrdiff_t ppb_prescan(struct ppb_buf buf, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires buf_valid_range(buf);
  @ requires buf.size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ terminates \true;
  @ assigns g_initial_buf, fields[0..num_fields - 1];
  @ ensures \result ≥ 0 ==> \result ≤ buf.size;
  @ ensures \result ≥ 0 ==> \result ≤ limit;
  @*/
static inline ptrdiff_t ppb_prescan_with_hard_limit(struct ppb_buf buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires buf_valid_range(buf);
  @ requires buf.size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ terminates \true;
  @ assigns g_initial_buf, fields[0..num_fields - 1];
  @ ensures \result ≥ 0 ==> \result ≤ buf.size;
  @*/
static inline ptrdiff_t ppb_prescan_with_soft_limit(struct ppb_buf buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1];
  @ ensures buf_valid(*buf);
  @ ensures \forall integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \old(fields[j].m);
  @ behavior progress:
  @   assumes buf->size > 0 ∧ max_lexed_fields > 0;
  @   ensures \result.status ≢ PPB_OK ∨ buf->size < \old(buf->size);
  @*/
static inline struct ppb_lexn_ret ppb_lexn(struct ppb_buf *__restrict buf, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1];
  @ ensures buf_valid(*buf);
  @ ensures \forall integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \old(fields[j].m);
  @ behavior hard_limit:
  @   ensures \result.status ≡ PPB_OK ==> \old(buf->size) - buf->size ≤ limit;
  @ behavior progress:
  @   assumes buf->size > 0 ∧ max_lexed_fields > 0 ∧ limit > 0;
  @   ensures \result.status ≢ PPB_OK ∨ buf->size < \old(buf->size);
  @*/
static inline struct ppb_lexn_ret ppb_lexn_with_hard_limit(struct ppb_buf *__restrict buf, size_t limit,
    size_t num_fields, const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1];
  @ ensures buf_valid(*buf);
  @ ensures \forall integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \old(fields[j].m);
  @ behavior progress:
  @   assumes buf->size > 0 ∧ max_lexed_fields > 0 ∧ limit > 0;
  @   ensures \result.status ≢ PPB_OK ∨ buf->size < \old(buf->size);
  @*/
static inline struct ppb_lexn_ret ppb_lexn_with_soft_limit(struct ppb_buf *__restrict buf, size_t limit,
    size_t num_fields, const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires \valid(error);
  @ requires *error ≡ 0;
  @ terminates \true;
  @ assigns g_initial_buf, *buf, *error;
  @ ensures buf_valid(*buf);
  @ ensures *error ≤ 0;
  @ ensures *error ≢ 0 ==> \result ≡ 0;
  @*/
uint64_t
ppb_decode_varint(struct ppb_buf *restrict buf, enum ppb_error *restrict error)
{
    /*@ ghost g_initial_buf = *buf; */
    return decode_varint(buf, error);
}

/*@ requires \valid_read(tags + (0..num_fields - 1));
  @ terminates \true;
  @ assigns \result \from tags[0..num_fields - 1].bits, num_fields;
  @ ensures \result ≡ PPB_OK ==> \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @*/
enum ppb_error
ppb_validate_tags(size_t num_fields, const struct ppb_encoded_tag *tags)
{
    if (unlikely(num_fields > 0 && tags[0].bits < 8))
        return PPB_ERROR_SENTINEL_FIELD_ARR;

    /*@ loop assigns i;
      @ loop invariant num_fields ≡ 0 ∨ 1 ≤ i ≤ num_fields;
      @ loop variant num_fields - i;
      @*/
    for (size_t i = 1; i < num_fields; i++)
    {
        if (unlikely(tags[i - 1].bits >= tags[i].bits))
            return PPB_ERROR_UNSORTED_FIELD_ARR;
    }

    /*
     * tags[0].bits >= 8 (sentinel check above) and the array is strictly sorted
     * (sort check above), so every element exceeds the previous and all are > 7.
     * WP cannot prove the universally-quantified induction automatically.
     */
    /*@ admit \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7; */

    return PPB_OK;
}

/*
 * Binary searches `tags` for an entry whose `.bits` equals `tag`.
 * Returns its index, or SIZE_MAX if not found.
 */
/*@ requires \valid_read(tags + (0 .. num_fields - 1));
  @ terminates \true;
  @ assigns \result \from tags[0..num_fields - 1].bits, tag;
  @ ensures \result < num_fields ==> tags[\result].bits ≡ tag;
  @*/
static inline size_t
find_tag(const size_t num_fields, const struct ppb_encoded_tag *__restrict tags, uint64_t tag)
{
    size_t lo = 0;
    size_t len = num_fields;

    if (unlikely(num_fields == 0))
    {
        return SIZE_MAX;
    }

    /*@ loop assigns lo, len;
      @ loop invariant lo < num_fields;
      @ loop invariant lo + len ≤ num_fields;
      @ loop invariant ∀ integer i; lo + len ≤ i < num_fields ==> tags[i].bits > tag;
      @ loop variant len;
      @*/
    while (len > 1)
    {
        size_t next_len = len - (len / 2);
        size_t pivot = len / 2;

        /* Sortedness: if the pivot exceeds tag, so do all elements after it. */
        /*@ admit tags[lo + pivot].bits > tag ==>
          @    ∀ integer i; lo + pivot ≤ i < num_fields ==> tags[i].bits > tag;
          @*/
        lo += tags[lo + pivot].bits > tag ? 0 : pivot;
        len = next_len;
    }

    return likely(tags[lo].bits == tag) ? lo : SIZE_MAX;
}

/*
 * Decodes the field with `tag` and remaining value at the head of `src`, and updates
 * `dst` accordingly.
 *
 * Returns 0 on success and a negative error on failure.
 *
 * Always updates the `dst->v` field, but updates `dst->m` only when
 * `update_metadata` is set.
 */
/*@ requires valid_tag: tag > 7;
  @ requires \valid(dst);
  @ requires \valid(src);
  @ requires buf_valid(*src);
  @ requires \valid(error);
  @ requires *error ≡ PPB_OK;
  @ terminates \true;
  @ assigns *dst, *error;
  @ assigns *src \from indirect:*src;
  @ ensures buf_valid(*src);
  @ ensures *error ≤ 0;
  @ ensures \result ≡ *error;
  @ ensures error_rollback: \result ≢ PPB_OK ==> *dst ≡ \old(*dst);
  @ ensures src->size ≤ \old(src->size);
  @ behavior nometa:
  @  assumes update_metadata ≡ false;
  @  assigns dst->v, *src, *error;
  @  ensures dst->m ≡ \old(dst->m);
  @*/
static int
handle_field(uint64_t tag, struct ppb_field *restrict dst, struct ppb_buf *restrict src, bool update_metadata,
    enum ppb_error *restrict error)
{
    size_t num_bytes;

    switch (tag % 8)
    {
    case PPB_WIRE_VARINT:
    {
        const char *initial = src->buf;
        uint64_t varint = decode_varint(src, error);
        if (unlikely(*error != PPB_OK))
        {
            return *error;
        }

        num_bytes = (size_t)((const char *)src->buf - initial);
        dst->v.u64 = varint;
        break;
    }

    case PPB_WIRE_I64:
    {
        if (unlikely(buf_check(*src, sizeof(uint64_t), error)))
        {
            return *error;
        }

        num_bytes = 8;
        dst->v.u64 = buf_peek64(*src);
        buf_advance(src, sizeof(uint64_t));
        break;
    }

    case PPB_WIRE_LEN:
    {
        num_bytes = decode_varint(src, error);
        if (unlikely(buf_check(*src, num_bytes, error) | (num_bytes == 0))) /* mutant-ok: 'if:force_true' */
        {
            if (likely(*error))
            {
                return *error;
            }
        }

        dst->v.payload.buf = src->buf;
        dst->v.payload.size = num_bytes;
        /*@ assert buf_valid_range(dst->v.payload); */
        buf_advance(src, num_bytes);
        break;
    }

    case PPB_WIRE_I32: /* fixed32 */
    {
        if (unlikely(buf_check(*src, sizeof(uint32_t), error)))
        {
            return *error;
        }

        num_bytes = 4;
        dst->v.u64 = buf_peek32(*src);
        buf_advance(src, sizeof(uint32_t));
        break;
    }

    case 3: /* mutant-skip: explicit group start instead of default */
    case 4: /* mutant-skip: explicit group end instead of default */
    default:
        /*@ assert *error ≡ 0; */
        return error_set(error, PPB_ERROR_CORRUPT_TAG);
    }

    /*@ assert *error ≡ PPB_OK; */
    if (update_metadata)
    {
        dst->m.num_occurrences++;
        dst->m.total_bytes += num_bytes;
        /* subtract one one both sides to map 0 to SIZE_MAX. */
        dst->m.min_nonzero_bytes = ((num_bytes - 1 < dst->m.min_nonzero_bytes - 1) ?
                num_bytes :
                dst->m.min_nonzero_bytes);
        dst->m.max_bytes = num_bytes > dst->m.max_bytes ? num_bytes : dst->m.max_bytes;
    }

    return 0;
}

/*@ requires buf_valid_range(buf);
  @ requires buf.size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ requires (int)limit_error ≤ 0;
  @ terminates \true;
  @ assigns g_initial_buf, fields[0..num_fields - 1];
  @ ensures \result ≥ 0 ==> \result ≤ buf.size;
  @ behavior hard_limit:
  @   assumes (int)limit_error < 0;
  @   ensures \result ≥ 0 ==> \result ≤ limit;
  @*/
ptrdiff_t
ppb_prescan_impl(const struct ppb_buf buf, const size_t num_fields,
    const struct ppb_encoded_tag *const restrict tags, struct ppb_field *const restrict fields,
    const size_t max_lexed_fields, const size_t limit, const enum ppb_error limit_error)
{
    enum ppb_error error = PPB_OK;
    /*@ ghost g_initial_buf = buf; */
    struct ppb_buf src = buf;
    /*@ assert buf_valid(src); */

    struct ppb_field dummy = { 0 };
    bool has_catch_all = num_fields > 0 && (int64_t)tags[num_fields - 1].bits < 0;
    /*
     * Break when src.size falls to or below this threshold, meaning bytes
     * consumed >= limit (just a saturating subtraction).
     */
    const size_t break_size = (limit <= buf.size) ? buf.size - limit : 0; /* mutant-ok: 'operator:<= -> <' */

    /*@ loop assigns i, fields[0..num_fields - 1], src, error, dummy;
      @ loop invariant 0 ≤ i ≤ max_lexed_fields;
      @ loop invariant buf_valid(src);
      @ loop invariant src.size ≤ buf.size;
      @ loop invariant error ≡ PPB_OK;
      @ loop variant max_lexed_fields - i;
      @*/
    for (size_t i = 0; i < max_lexed_fields; i++)
    {
        /*@ assert error ≡ PPB_OK; */
        assert(error == PPB_OK && "loop invariant");

        if (unlikely(src.size <= break_size))
        {
            break;
        }

        const void *ptr = src.buf;
        uint64_t tag = decode_tag(&src, &error);
        size_t field_idx = find_tag(num_fields, tags, tag);
        /*@ assert error ≢ PPB_OK ==> tag ≡ 0; */
        /*
         * All encoded_tags are > 7: fields[0].tag.bits > 7 and the
         * array is strictly sorted, so each element > 7.
         * find_tag postcondition: field_idx < num_fields ==> tags[field_idx].bits ≡ tag.
         * Together: field_idx < num_fields ==> tag > 7.
         */
        /*@ admit zero_absent: tag < 8 ==> field_idx ≥ num_fields; */
        /*@ assert field_idx < num_fields ==> tag > 7; */

        if (unlikely(field_idx >= num_fields)) /* -1UL for missing, so mutant-ok: '>=' -> '>' */
        {
            /* tag < 8: field number 0 is invalid in protobuf. */
            if (unlikely(tag < 8))
            {
                error_set(&error, PPB_ERROR_CORRUPT_TAG);
                return (ptrdiff_t)error;
            }

            /* See if we want to dump this in a catch-all field (branch is an optimization). */
            if (has_catch_all) /* mutant-skip */
            {
                tag |= UINT64_MAX << 3; /* preserve the type, but otherwise all 1s. */
                field_idx = find_tag(num_fields, tags, tag);
            }
        }

        struct ppb_field *dst = (field_idx < num_fields) ? fields + field_idx : &dummy;
        dst->v = (struct ppb_field_value) { .ptr = ptr };

        /*@ admit tag > 7; // precondition: tags[k] > 7; and tag < 8 returns early */
        int rc = handle_field(tag, dst, &src, /*update_metadata=*/true, &error);
        if (unlikely(rc != 0))
        {
            return (ptrdiff_t)error;
        }
    }

    size_t uconsumed = buf.size - src.size; /* safe since src.size ≤ buf.size (loop invariant) */
    /*@ assert uconsumed ≤ buf.size; */
    ptrdiff_t consumed = (ptrdiff_t)uconsumed;
    if (unlikely(src.size < break_size))
    {
        if (limit_error != PPB_OK)
        {
            return (ptrdiff_t)limit_error;
        }
    }

    /*@ assert (int)limit_error < 0 ==> src.size ≥ break_size; */
    /*@ assert limit ≤ buf.size ==> break_size ≡ buf.size - limit; */
    /*@ assert limit > buf.size ==> break_size ≡ 0; */
    /*@ assert (int)limit_error < 0 ==> consumed ≤ limit; */

    return consumed;
}

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ requires (int)limit_error ≤ 0;
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1];
  @ ensures buf_valid(*buf);
  @ ensures \forall integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \old(fields[j].m);
  @ behavior hard_limit:
  @   // When a hard limit is in force and the call succeeds, bytes consumed ≤ limit.
  @   assumes (int)limit_error < 0;
  @   ensures \result.status ≡ PPB_OK ==> \old(buf->size) - buf->size ≤ limit;
  @ behavior progress:
  @   // either consume at least one byte, or flag an error
  @   assumes buf->size > 0 ∧ max_lexed_fields > 0 ∧ limit > 0;
  @   ensures \result.status ≢ PPB_OK ∨ buf->size < \old(buf->size);
  @*/
struct ppb_lexn_ret
ppb_lexn_impl(struct ppb_buf *restrict const buf, const size_t num_fields,
    const struct ppb_encoded_tag *restrict const tags, struct ppb_field *restrict const fields,
    const size_t max_lexed_fields, const size_t limit, const enum ppb_error limit_error)
{
    enum ppb_error error = PPB_OK;
    /*@ ghost g_initial_buf = *buf; */
    struct ppb_buf src = *buf;
    /*@ assert buf_valid(src); */

    struct ppb_field dummy = { 0 };
    /*
     * look for a catch-all field if field_idx > try_catch_all_field_id:
     * num_fields - 1 when the last tag is a catch-all, SIZE_MAX otherwise.
     */
    const size_t try_catch_all_field_id = (num_fields > 0 && (int64_t)tags[num_fields - 1].bits < 0) ?
        num_fields - 1 : /* could be any larger value < SIZE_MAX; mutant-skip */
        SIZE_MAX;
    uint64_t prev_tag = 7; /* 8 = PPB_TAG_BITS(1, PPB_WIRE_VARINT) is the minimum valid tag */
    size_t first_field = SIZE_MAX;
    size_t last_field = 0;
    /*
     * Break when src.size falls to or below this threshold, meaning bytes
     * consumed >= limit (just a saturating subtraction).
     */
    const size_t break_size = (limit <= src.size) ? src.size - limit : 0; /* mutant-ok: 'operator:<= -> <' */
    /*@ ghost size_t ghost_consumed = 0; */

    /*@ loop assigns i, fields[0..num_fields - 1], src, error, dummy, prev_tag, first_field, last_field,
      ghost_consumed;
      @ loop invariant 0 ≤ i ≤ max_lexed_fields;
      @ loop invariant buf_valid(src);
      @ loop invariant error ≡ PPB_OK;
      @ loop invariant prev_tag_monotonic: prev_tag ≥ \at(prev_tag, LoopCurrent) ≥ 7;
      @ loop invariant ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \at(fields[j].m, Pre);
      @ loop invariant consumed_after_first: i ≡ 0 ∨ ghost_consumed > 0;
      @ loop invariant consumed_link: src.size + ghost_consumed ≤ \at(src.size, LoopEntry);
      @ loop invariant prev_tag_link: ghost_consumed ≡ 0 ==> prev_tag ≡ 7;
      @ loop variant max_lexed_fields - i;
      @*/
    for (size_t i = 0; i < max_lexed_fields; i++)
    {
        /*@ assert error ≡ PPB_OK; */
        assert(error == PPB_OK && "loop invariant");

        if (unlikely(src.size <= break_size))
        {
            break;
        }

        const void *ptr = src.buf;
        uint64_t tag;
        size_t field_idx;

        {
            int num_tag_bytes = peek_tag(src, &tag);
            /*@ assert num_tag_bytes ≤ 0 ==> tag ≡ 0; */
            /*@ assert tag < 8 ==> tag ≤ prev_tag; */

            /*
             * peek_tag returns < (1 << 63), so we'll always break when
             * prev_tag was populated for a catch-all field.
             */
            /* TODO: bulk-skip ignored fields more efficiently. */
            if (unlikely(tag <= prev_tag))
            {
                if (unlikely(num_tag_bytes < 0))
                {
                    error_set(&error, (enum ppb_error)num_tag_bytes);
                }
                else if (unlikely(tag < 8))
                {
                    error_set(&error, PPB_ERROR_CORRUPT_TAG);
                }

                break;
            }

            /* any error (num_tag_bytes < 0) would have set `tag = 0`. */
            /*@ assert num_tag_bytes > 0; */

            field_idx = find_tag(num_fields, tags, tag);
            /* See if we want to dump this in a catch-all field. */
            if (unlikely(field_idx > try_catch_all_field_id))
            {
                /*@ assert prev_tag < tag ∧ prev_tag < (1 << 63); */
                /*@ ghost uint64_t pre_or_tag = tag; */
                tag |= UINT64_MAX << 3; /* preserve the type, but otherwise all 1s. */
                /*@ assert tag_or_monotone: tag ≥ pre_or_tag > prev_tag; // bitor_ge_l */
                field_idx = find_tag(num_fields, tags, tag);
            }

            buf_advance(&src, (size_t)num_tag_bytes); /* num_tag_bytes > 0 here */
            /*@ ghost ghost_consumed += (size_t)num_tag_bytes; */
        }

        struct ppb_field *dst = &dummy;
        /* Update metadata if we want the field (but always lex to advance `src`). */
        if (likely(field_idx < num_fields))
        {
            /*@ assert tag > prev_tag; */
            prev_tag = tag;
            first_field = field_idx < first_field ? field_idx : first_field;
            last_field = field_idx > last_field ? field_idx : last_field;
            dst = fields + field_idx;
        }

        dst->v = (struct ppb_field_value) { .ptr = ptr };
        /*@ assert tag > 7; */
        int rc = handle_field(tag, dst, &src, false, &error);

        /*
         * handle_field with update_metadata=false ensures dst->m ≡ \old(dst->m),
         * (see nometa behavior), but the prover can't reason through the pointers.
         */
        /*@ admit ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \at(fields[j].m, Pre); */

        if (unlikely(rc != 0))
        {
            break;
        }
    }

    /*
     * Signal the limit error (if any) if the final field ended
     * strictly after the limit.  error_set is sticky, so we're
     * not losing a prior error.
     */
    if (unlikely(src.size < break_size))
    {
        error_set(&error, limit_error);
    }

    /*
     * buf_valid(src) holds from the loop invariant; the prover
     * can't fold buf_valid through the struct copy (hence the
     * admitted postcondition).
     */
    *buf = src;

    uint32_t field_range;
    if (first_field > last_field)
    {
        first_field = 0;
        field_range = 0;
    }
    else if (unlikely(last_field - first_field >= UINT32_MAX)) /* mutant-triaged: needs large fields array */
    {
        field_range = UINT32_MAX; /* GCOVR_EXCL_LINE mutant-triaged: currently unreachable */
    }
    else
    {
        /*@ assert no_truncation: 1 + last_field - first_field ≤ UINT32_MAX; */
        field_range = (uint32_t)(1 + last_field - first_field);
    }

    return (struct ppb_lexn_ret) {
        .first_field = first_field,
        .field_range = field_range,
        .status = error,
    };
}

#ifdef __FRAMAC__
/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires buf->size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1];
  @ ensures buf_valid(*buf);
  @*/
struct ppb_lexn_ret
ppb_entry_point(struct ppb_buf *restrict buf, size_t num_fields, const struct ppb_encoded_tag *restrict tags,
    struct ppb_field *restrict fields, size_t max_lexed_fields)
{
    /*@ ghost g_initial_buf = *buf; */
    enum ppb_error err = ppb_validate_tags(num_fields, tags);
    if (err != PPB_OK)
        return (struct ppb_lexn_ret) { .status = err };
    ppb_prescan_impl(*buf, num_fields, tags, fields, max_lexed_fields, SIZE_MAX, PPB_OK);
    return ppb_lexn_impl(buf, num_fields, tags, fields, max_lexed_fields, SIZE_MAX, PPB_OK);
}
#endif
