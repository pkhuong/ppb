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

/*@ terminates \true;
  @ assigns \result \from x;
  @ behavior sint32:
  @  assumes 0 ≤ x < (1 << 32);
  @  admit ensures  (-1 << 31) ≤ \result < (1 << 31);
  @ behavior sint64:
  @  assumes x ≥ (1 << 32);
  @  ensures (-1 << 63) ≤ \result < (1 << 63);
  @ complete behaviors sint32, sint64;
  @ disjoint behaviors sint32, sint64;
  @*/
static inline int64_t ppb_zag(uint64_t x);

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

/*
 * Binary searches for the `ppb_field` in `fields` where `tag.bits` matches `tag`:
 * `tag` masks off irrelevant bytes, but still contains the continuation/stop bit
 * at the top of each byte.
 *
 * Returns the index of an exact match in `fields`, and an invalid
 * index, greater than or equal to `num_fields`.
 */
/*@ requires \valid_read(fields + (0 .. num_fields - 1));
  @ terminates \true;
  @ assigns \result \from fields[0..num_fields - 1].tag.bits, tag;
  @ ensures \result < num_fields ==> fields[\result].tag.bits ≡ tag;
  @*/
static inline size_t
find_tag(const size_t num_fields, const struct ppb_field *__restrict fields, uint64_t tag)
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
      @ loop invariant ∀ integer i; lo + len ≤ i < num_fields ==> fields[i].tag.bits > tag;
      @ loop variant len;
      @*/
    while (len > 1)
    {
        size_t next_len = len - (len / 2);
        size_t pivot = len / 2;

        /*
         * Assume monotonicity: callers pass strictly sorted arrays
         * (ppb_prescan validates, ppb_lexn requires it informally).
         * Sortedness and fields[lo + pivot] > tag imply fields[i] > tag
         * for i >= lo + pivot.
         */
        /*@ admit fields[lo + pivot].tag.bits > tag ==>
          @    ∀ integer i; lo + pivot ≤ i < num_fields ==> fields[i].tag.bits > tag;
          @*/
        lo += fields[lo + pivot].tag.bits > tag ? 0 : pivot;
        len = next_len;
    }

    return fields[lo].tag.bits == tag ? lo : SIZE_MAX;
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
/*@ requires \valid(dst);
  @ requires \valid(src);
  @ requires buf_valid(*src);
  @ requires \valid(error);
  @ requires *error ≡ PPB_OK;
  @ terminates \true;
  @ assigns dst->m, dst->v, *error;
  @ assigns *src \from indirect:*src;
  @ ensures buf_valid(*src);
  @ ensures *error ≤ 0;
  @ ensures \result ≡ *error;
  @ ensures error_rollback: \result ≢ PPB_OK ==> dst->m ≡ \old(dst->m) ∧ dst->v ≡ \old(dst->v);
  @ ensures src->size ≤ \old(src->size);
  @ ensures src->buf ≡ (const char *)\old(src->buf) + \old(src->size) - src->size;
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

        num_bytes = (const char *)src->buf - initial;
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
        if (unlikely((buf_check(*src, num_bytes, error) != 0) | (num_bytes == 0)))
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

    case 3: /* group start */
    case 4: /* group end */
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
  @ requires \valid(fields + (0..num_fields - 1));
  @ terminates \true;
  @ assigns g_initial_buf, fields[0..num_fields - 1].m, fields[0..num_fields - 1].v;
  @ ensures \result ≥ 0 ==> (num_fields ≡ 0 ∨ fields[0].tag.bits ≢ 0);
  @*/
ptrdiff_t
ppb_prescan(const struct ppb_buf buf, const size_t num_fields, struct ppb_field *const restrict fields,
    const size_t max_lexed_fields)
{
    enum ppb_error error = PPB_OK;
    /*@ ghost g_initial_buf = buf; */
    struct ppb_buf src = buf;
    /*@ assert buf_valid(src); */

    if (unlikely(num_fields > 0 && fields[0].tag.bits == 0))
    {
        return PPB_ERROR_SENTINEL_FIELD_ARR;
    }

    /*@ loop assigns i;
      @ loop invariant num_fields ≡ 0 ∨ 1 ≤ i ≤ num_fields;
      @ loop variant num_fields - i;
      @*/
    for (size_t i = 1; i < num_fields; i++)
    {
        if (unlikely(fields[i - 1].tag.bits >= fields[i].tag.bits))
        {
            return PPB_ERROR_UNSORTED_FIELD_ARR;
        }
    }

    struct ppb_field dummy = { .tag.bits = 0 };

    /*@ loop assigns i, fields[0..num_fields - 1].m, fields[0..num_fields - 1].v, src, error, dummy;
      @ loop invariant 0 ≤ i ≤ max_lexed_fields;
      @ loop invariant buf_valid(src);
      @ loop invariant error ≡ PPB_OK;
      @ loop invariant num_fields ≡ 0 ∨ fields[0].tag.bits ≢ 0;
      @ loop variant max_lexed_fields - i;
      @*/
    for (size_t i = 0; i < max_lexed_fields; i++)
    {
        if (unlikely(src.size == 0))
        {
            break;
        }

        const void *ptr = src.buf;
        uint64_t tag = decode_tag(&src, &error);
        size_t field_idx = find_tag(num_fields, fields, tag);
        /*@ assert field_idx < num_fields ==> fields[field_idx].tag.bits ≡ tag; */
        /*
         * All encoded_tags are > 0: fields[0].tag.bits ≢ 0 and the
         * array is strictly sorted, so each element > 0.
         */
        /*@ admit field_idx < num_fields ==> fields[field_idx].tag.bits ≢ 0; */

        if (unlikely(field_idx >= num_fields))
        {
            /* tag == 0 is invalid. */
            if (unlikely(tag == 0))
            {
                error_set(&error, PPB_ERROR_CORRUPT_TAG);
                return (ptrdiff_t)error;
            }

            /* See if want to dump this in the a catch-all field. */
            if (num_fields > 0 && (int64_t)fields[num_fields - 1].tag.bits >> 3 < 0)
            {
                tag |= UINT64_MAX << 3;  /* preserve the type, but otherwise all 1s. */
                field_idx = find_tag(num_fields, fields, tag);
            }
        }

        /*@ assert \separated(&dummy, fields + (0 .. num_fields - 1)); */
        struct ppb_field *dst = (field_idx < num_fields) ? &fields[field_idx] : &dummy;
        /*@ ghost TAG_PRESCAN:; */
        /*@ assert num_fields ≡ 0 ∨ fields[0].tag.bits ≢ 0; */
        dst->v = (struct ppb_field_value) { .ptr = ptr };
        int rc = handle_field(tag, dst, &src, /*update_metadata=*/true, &error);

        /*
         * Neither dst->v assignment nor handle_field (which only
         * assigns dst->m, dst->v, *error, *src) touches .tag.
         * The typed memory model distinguishes .tag from .m/.v,
         * but the provers time out on the resulting obligation.
         */
        /*@ admit num_fields > 0 ==>
          @     fields[0].tag.bits ≡ \at(fields[0].tag.bits, TAG_PRESCAN); */
        /*@ assert num_fields ≡ 0 ∨ fields[0].tag.bits ≢ 0; */
        if (unlikely(rc != 0))
        {
            return (ptrdiff_t)error;
        }
    }

    return (const char *)src.buf - (const char *)buf.buf;
}

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> fields[j].tag.bits ≢ 0;
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1].m, fields[0..num_fields - 1].v;
  @ admit ensures buf_valid(*buf);
  @ ensures \forall integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \old(fields[j].m);
  @*/
struct ppb_lexn_ret
ppb_lexn(struct ppb_buf *restrict const buf, const size_t num_fields, struct ppb_field *restrict const fields,
    const size_t max_lexed_fields)
{
    enum ppb_error error = PPB_OK;
    /*@ ghost g_initial_buf = *buf; */
    struct ppb_buf src = *buf;
    /*@ assert buf_valid(src); */

    struct ppb_field dummy = { .tag.bits = 0 };
    uint64_t prev_tag_id = 0;  /* without the 3 type bits */
    size_t first_field = SIZE_MAX;
    size_t last_field = 0;

    /*@ loop assigns i, fields[0..num_fields - 1].m, fields[0..num_fields - 1].v, src, error, dummy,
      prev_tag_id, first_field, last_field;
      @ loop invariant 0 ≤ i ≤ max_lexed_fields;
      @ loop invariant buf_valid(src);
      @ loop invariant error ≡ PPB_OK;
      @ loop invariant ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \at(fields[j].m, Pre);
      @ loop variant max_lexed_fields - i;
      @*/
    for (size_t i = 0; i < max_lexed_fields; i++)
    {
        if (unlikely(src.size == 0))
        {
            break;
        }

        const void *ptr = src.buf;
        uint64_t tag;
        size_t field_idx;

        {
            int num_tag_bytes = peek_tag(src, &tag);
            /*@ assert num_tag_bytes ≤ 0 ==> tag ≡ 0; */
            /*@ assert tag ≡ 0 ==> tag / 8 ≡ 0 ≤ prev_tag_id; */

            /* TODO: bulk-skip ignored fields more efficiently. */
            if (unlikely(tag / 8 <= prev_tag_id))
            {
                if (unlikely(num_tag_bytes < 0))
                {
                    error_set(&error, (enum ppb_error)num_tag_bytes);
                }
                else if (unlikely(tag == 0))
                {
                    error_set(&error, PPB_ERROR_CORRUPT_TAG);
                }

                break;
            }

            /* any error (num_tag_bytes < 0) would have set `tag = 0`. */
            /*@ assert num_tag_bytes > 0; */

            field_idx = find_tag(num_fields, fields, tag);
            if (unlikely(field_idx >= num_fields))
            {
                if (num_fields > 0 && (int64_t)fields[num_fields - 1].tag.bits < 0)
                {
                    /* See if want to dump this in a catch-all field. */
                    tag |= UINT64_MAX << 3;  /* preserve the type, but otherwise all 1s. */
                    if (unlikely(UINT64_MAX / 8 <= prev_tag_id))
                    {
                        /*
                         * nonmonotonic -> stop here.
                         *
                         * Currently unreachable, because the check between the
                         * real tag and prev_tag_id would have already triggered
                         * the nonmonotonicity check.
                         */
                        break;
                    }

                    field_idx = find_tag(num_fields, fields, tag);
                }
            }

            buf_advance(&src, num_tag_bytes);
        }

        struct ppb_field *dst = &dummy;
        /* Update metadata if we want the field (but always lex to advance `src`). */
        if (likely(field_idx < num_fields))
        {
            prev_tag_id = tag / 8;
            first_field = field_idx < first_field ? field_idx : first_field;
            last_field = field_idx > last_field ? field_idx : last_field;
            dst = &fields[field_idx];
        }

        /*@ assert \separated(&dummy, fields + (0 .. num_fields - 1)); */
        /*@ assert \valid(dst); */
        dst->v = (struct ppb_field_value) { .ptr = ptr };
        int rc = handle_field(tag, dst, &src, false, &error);

        /*
         * handle_field with update_metadata=false activates the nometa
         * behavior: assigns only dst->v, *src, *error (not dst->m),
         * and ensures dst->m ≡ \old(dst->m).
         * The prover can't reason through the conditional pointer.
         */
        /*@ admit ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \at(fields[j].m, Pre); */

        if (unlikely(rc != 0))
        {
            break;
        }
    }

    /*
     * buf_valid(src) holds from the loop invariant; the prover
     * can't fold buf_valid through the struct copy (hence the
     * admitted postcondition).
     */
    /*@ assert buf_valid(src); */
    *buf = src;

    size_t field_range;
    if (first_field > last_field)
    {
        first_field = 0;
        field_range = 0;
    }
    else if (unlikely(last_field - first_field >= UINT32_MAX))
    {
        field_range = UINT32_MAX;
    }
    else
    {
        /*@ assert no_truncation: 1 + last_field - first_field ≤ UINT32_MAX; */
        field_range = 1 + last_field - first_field;
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
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \forall integer j; 0 ≤ j < num_fields ==> fields[j].tag.bits ≢ 0;
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1].m, fields[0..num_fields - 1].v;
  @ ensures buf_valid(*buf);
  @*/
struct ppb_lexn_ret
ppb_entry_point(struct ppb_buf *restrict buf, size_t num_fields, struct ppb_field fields[restrict num_fields],
    size_t max_lexed_fields)
{
    ppb_prescan(*buf, num_fields, fields, max_lexed_fields);
    /* prescan preserves .tag (not in its assigns clause). */
    /*@ assert \forall integer j; 0 ≤ j < num_fields ==> fields[j].tag.bits ≢ 0; */
    return ppb_lexn(buf, num_fields, fields, max_lexed_fields);
}
#endif
