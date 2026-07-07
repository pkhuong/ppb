#include "ppb/ppb.h"

#include "buf.h"
#include "cc.h"
#include "sat_sub.h"
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
static_assert(PPB_TAG_BITS(33554432, PPB_WIRE_VARINT) == 0x0180808080ULL,
    "field 33554432, varint: [0x80, 0x80, 0x80, 0x80, 0x01]");
static_assert(PPB_ENCODE_VARINT(UINT32_MAX) == 0x0FFFFFFFFFULL, "UINT32_MAX: [0xFF, 0xFF, 0xFF, 0xFF, 0x0F]");

/* Eight-byte encoding. */
static_assert(PPB_ENCODE_VARINT(1ULL << 49) == 0x0180808080808080ULL,
    "1<<49: [0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01]");
static_assert(PPB_ENCODE_VARINT((1ULL << 56) - 1) == 0x7FFFFFFFFFFFFFFFULL,
    "(1<<56)-1: [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F]");

/* Catch-all. */
static_assert(PPB_TAG_BITS(-1, PPB_WIRE_VARINT) == ((UINT64_MAX << 3) | 0), "catch-all, varint");
static_assert(PPB_TAG_BITS(-1, PPB_WIRE_I32) == ((UINT64_MAX << 3) | 5), "catch-all, I32");

static_assert(PTRDIFF_MAX <= SIZE_MAX / 2,
    "we rely on this inequality to steal one bit from ppb_field_meta::num_occurrences");

/* no-op, only used to hide values from the compiler. */
#define ASM(...) __asm__("" : __VA_ARGS__)

#ifdef __FRAMAC__
#undef ASM
#define ASM(...)  /* empty asm isn't supported by frama-c */
#endif

/*
 * Bitwise OR is monotone: OR can only set bits, never clear them.
 * WP lacks a built-in axiom for this, so we admit it globally.
 */
/*@ admit lemma bitor_ge_l: ∀ uint64_t a, b; (a | b) ≥ a; */

/*@ terminates \true;
  @ assigns \result \from x;
  @ behavior sint32:
  @  assumes 0 ≤ x < (1 << 32);
  @  // Tested by test_zag() boundary cases in tests/test_ppb.c.
  @  admit ensures sint32_in_range: (-1 << 31) ≤ \result < (1 << 31);
  @ behavior sint64:
  @  assumes x ≥ (1 << 32);
  @  ensures (-1 << 63) ≤ \result < (1 << 63);
  @ complete behaviors sint32, sint64;
  @ disjoint behaviors sint32, sint64;
  @*/
static inline int64_t ppb_zag(uint64_t x);

/*@ terminates \true;
  @ assigns \result \from x;
  @*/
static inline int32_t ppb_zag32(uint32_t x);

/*@ requires buf_valid_range(buf);
  @ requires buf.size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ terminates \true;
  @ assigns g_initial_buf, fields[0..num_fields - 1];
  @ behavior well_formed:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @  ensures \result ≥ 0 ==> \result ≤ buf.size;
  @*/
static inline ptrdiff_t ppb_prescan(struct ppb_buf buf, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires buf_valid_range(buf);
  @ requires buf.size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ terminates \true;
  @ assigns g_initial_buf, fields[0..num_fields - 1];
  @ behavior well_formed:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @  ensures \result ≥ 0 ==> \result ≤ buf.size;
  @  ensures \result ≥ 0 ==> \result ≤ limit;
  @*/
static inline ptrdiff_t ppb_prescan_with_hard_limit(struct ppb_buf buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires buf_valid_range(buf);
  @ requires buf.size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ terminates \true;
  @ assigns g_initial_buf, fields[0..num_fields - 1];
  @ behavior well_formed:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @  ensures \result ≥ 0 ==> \result ≤ buf.size;
  @*/
static inline ptrdiff_t ppb_prescan_with_soft_limit(struct ppb_buf buf, size_t limit, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires buf->size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1];
  @ ensures buf_valid(*buf);
  @ ensures ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \old(fields[j].m);
  @ ensures \result.first_field + \result.field_range ≤ num_fields;
  @
  @ behavior well_formed:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ behavior progress:
  @  assumes buf->size > 0 ∧ max_lexed_fields > 0;
  @  ensures \result.status ≢ PPB_OK ∨ buf->size < \old(buf->size);
  @*/
static inline struct ppb_lexn_ret ppb_lexn(struct ppb_buf *__restrict buf, size_t num_fields,
    const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires buf->size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1];
  @ ensures buf_valid(*buf);
  @ ensures ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \old(fields[j].m);
  @ ensures \result.first_field + \result.field_range ≤ num_fields;
  @
  @ behavior well_formed:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ behavior hard_limit:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @  ensures \result.status ≡ PPB_OK ==> \old(buf->size) - buf->size ≤ limit;
  @ behavior progress:
  @  assumes buf->size > 0 ∧ max_lexed_fields > 0 ∧ limit > 0;
  @  ensures \result.status ≢ PPB_OK ∨ buf->size < \old(buf->size);
  @*/
static inline struct ppb_lexn_ret ppb_lexn_with_hard_limit(struct ppb_buf *__restrict buf, size_t limit,
    size_t num_fields, const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires buf->size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1];
  @ ensures buf_valid(*buf);
  @ ensures ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \old(fields[j].m);
  @ ensures \result.first_field + \result.field_range ≤ num_fields;
  @
  @ behavior well_formed:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @ behavior progress:
  @  assumes buf->size > 0 ∧ max_lexed_fields > 0 ∧ limit > 0;
  @  ensures \result.status ≢ PPB_OK ∨ buf->size < \old(buf->size);
  @*/
static inline struct ppb_lexn_ret ppb_lexn_with_soft_limit(struct ppb_buf *__restrict buf, size_t limit,
    size_t num_fields, const struct ppb_encoded_tag *__restrict tags, struct ppb_field *__restrict fields,
    size_t max_lexed_fields);

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires buf->size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid(error);
  @ terminates \true;
  @ assigns g_initial_buf, *buf, *error;
  @ ensures buf_valid(*buf);
  @ behavior well_formed:
  @  assumes *error ≡ 0;
  @  ensures *error ≤ 0;
  @  ensures *error ≢ 0 ==> \result ≡ 0;
  @*/
uint64_t
ppb_decode_varint(struct ppb_buf *restrict buf, enum ppb_error *restrict error)
{
    /*@ ghost g_initial_buf = *buf; */
    return decode_varint(buf, error, /*on_error=*/0);
}

/*@ requires \valid_read(tags + (0..num_fields - 1));
  @ terminates \true;
  @ assigns \result \from tags[0..num_fields - 1].bits, num_fields;
  @ ensures \result ≡ PPB_OK ==> ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
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
    /*@ admit tags_above_seven_induction:
      @    ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
      @*/

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
  @ behavior well_formed:
  @  assumes ∀ integer i, j; 0 ≤ i < j < num_fields ==> tags[i].bits < tags[j].bits;
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
      @ for well_formed: loop invariant ∀ integer i; lo + len ≤ i < num_fields ==> tags[i].bits > tag;
      @ loop variant len;
      @*/
    while (len > 1)
    {
        size_t next_len = len - (len / 2);
        size_t pivot = len / 2;

        /* Sortedness: if the pivot exceeds tag, so do all elements after it. */
        /*@ for well_formed: admit pivot_dominates_suffix: tags[lo + pivot].bits > tag ==>
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
/*@ requires \valid(dst);
  @ requires \valid(src);
  @ requires buf_valid(*src);
  @ requires \valid(error);
  @ terminates \true;
  @ assigns *dst, *error;
  @ assigns *src \from indirect:*src;
  @ ensures buf_valid(*src);
  @ ensures src->size ≤ \old(src->size);
  @ behavior well_formed:
  @  assumes valid_tag: tag > 7;
  @  assumes *error ≡ PPB_OK;
  @  ensures *error ≤ 0;
  @  ensures \result ≡ *error;
  @  ensures error_rollback: \result ≢ PPB_OK ==> *dst ≡ \old(*dst);
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
    uint64_t new_value;  /* update for v.u64 */

    switch (tag % 8)
    {
    case PPB_WIRE_VARINT:
    {
        const char *initial = src->buf;
        new_value = decode_varint(src, error, /*on_error=*/0);
        if (unlikely(*error != PPB_OK))
        {
            return *error;
        }

        num_bytes = (size_t)((const char *)src->buf - initial);
        break;
    }

    case PPB_WIRE_I64:
    {
        if (unlikely(buf_check(*src, sizeof(uint64_t), error)))
        {
            return *error;
        }

        num_bytes = 8;
        new_value = buf_peek64(*src);
        buf_advance(src, sizeof(uint64_t));
        break;
    }

    case PPB_WIRE_LEN:
    {
        static_assert(UINT64_MAX > PTRDIFF_MAX, "we assume UINT64_MAX always fails buf_check");

        uint64_t len = decode_varint(src, error, /*on_error=*/UINT64_MAX);
        if (unlikely(buf_check(*src, len, error)))
        {
            return *error;
        }

        /*@ assert len ≤ src->size ≤ SIZE_MAX; */
        num_bytes = (size_t)len;
        new_value = 0; /* mutant-skip: delete is UB, but passes tests; -Wmaybe-uninitialized flags anyway. */
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
        new_value = buf_peek32(*src);
        buf_advance(src, sizeof(uint32_t));
        break;
    }

    case 3: /* mutant-skip: explicit group start instead of default */
    case 4: /* mutant-skip: explicit group end instead of default */
    default:
        /*@ for well_formed: assert *error ≡ 0; */
        return error_set(error, PPB_ERROR_CORRUPT_TAG);
    }

    uint64_t old_value = dst->v.u64;
    dst->v.u64 = new_value;

    /*@ for well_formed: assert *error ≡ PPB_OK; */
    if (update_metadata)
    {
        /* check for diff only on subsequent hits, first one isn't a diff */
        old_value = dst->m.num_occurrences > 0 ? old_value : new_value;
        ASM("+r"(old_value)); /* mutant-skip: opaque no-op to prevent if/if conversion (we want cmov + if) */

        dst->m.num_occurrences++;
        if (unlikely(old_value != new_value))
        {
            dst->m.lost_distinct_u64 = 1;
        }

        dst->m.total_bytes += num_bytes;
        /* subtract one on both sides to map 0 to SIZE_MAX. */
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
  @ terminates \true;
  @ assigns g_initial_buf, fields[0..num_fields - 1];
  @ behavior well_formed:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;  // needed for admits.
  @  assumes limit_error ≤ 0;
  @  ensures \result ≥ 0 ==> \result ≤ buf.size;
  @ behavior hard_limit:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @  assumes limit_error < 0;
  @  ensures \result ≥ 0 ==> \result ≤ limit;
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
     * consumed >= limit.
     */
    const size_t break_size = saturating_subzu(buf.size, limit);

    /*@ loop assigns i, fields[0..num_fields - 1], src, error, dummy;
      @ loop invariant 0 ≤ i ≤ max_lexed_fields;
      @ loop invariant buf_valid(src);
      @ loop invariant src.size ≤ buf.size;
      @ for well_formed, hard_limit: loop invariant error ≡ PPB_OK;
      @ loop variant max_lexed_fields - i;
      @*/
    for (size_t i = 0; i < max_lexed_fields; i++)
    {
        /*@ for well_formed: assert error ≡ PPB_OK; */
        assert(error == PPB_OK && "loop invariant");

        if (unlikely(src.size <= break_size))
        {
            break;
        }

        const void *ptr = src.buf;
        uint64_t tag = decode_tag(&src, &error);
        size_t field_idx = find_tag(num_fields, tags, tag);
        /*@ for well_formed: assert error ≢ PPB_OK ==> tag ≡ 0; */
        /*
         * All encoded_tags are > 7: fields[0].bits > 7 and the array
         * is strictly sorted, so each element > 7.
         * find_tag postcondition: field_idx < num_fields ==> tags[field_idx].bits ≡ tag.
         * Together: field_idx < num_fields ==> tag > 7.
         */
        /*@ for well_formed: admit zero_absent: tag < 8 ==> field_idx ≥ num_fields; */
        /*@ for well_formed: assert field_idx < num_fields ==> tag > 7; */

        if (unlikely(field_idx >= num_fields)) /* -1UL for missing, so mutant-ok: '>=' -> '>' */
        {
            /*
             * Two fatal wire-format checks on an unmatched tag, done here in
             * prescan (a standalone ppb_lexn only looks for canonical zero):
             *   - Field number 0 (canonical `0x00-0x07` or any overlong encoding):
             *     the field-number mask keeps the field-number bits of every
             *     tag byte, so a zero result means field number 0.
             *   - A tag longer than 5 bytes, or one whose value exceeds
             *     UINT32_MAX.  A valid protobuf tag encodes a field number
             *     at most 2**29 - 1, so it fits in a uint32_t and in <= 5
             *     varint bytes.  libprotobuf truncates <= 5-byte tags
             *     to 32 bits; we reject instead of adopting that design.
             */
            uint64_t tag_index_mask = (127UL * ((uint64_t)-1 / 255UL)) & ~(uint64_t)7;
            uint64_t tag_range_mask = ~PPB_ENCODE_VARINT(UINT32_MAX);
            if (unlikely(((tag & tag_index_mask) == 0) | ((tag & tag_range_mask) != 0)))
            {
                error_set(&error, PPB_ERROR_CORRUPT_TAG);
                return (ptrdiff_t)error;
            }

            /*@ assert (tag & tag_index_mask) ≢ 0; */
            /*@ assert (7 & tag_index_mask) ≡ 0; */
            /*@ admit bitwise_to_int_transfer: tag > 7; */

            /* See if we want to dump this in a catch-all field (branch is an optimization). */
            if (has_catch_all) /* mutant-skip */
            {
                tag |= UINT64_MAX << 3; /* preserve the type, but otherwise all 1s. */
                field_idx = find_tag(num_fields, tags, tag);
            }
        }

        struct ppb_field *dst = (field_idx < num_fields) ? fields + field_idx : &dummy;
        dst->v.ptr = ptr;

        /*@ for well_formed:  // precondition: tags[k] > 7; and tag < 8 returns early
          @    admit tag_above_seven_after_match: tag > 7;
          @*/
        int rc = handle_field(tag, dst, &src, /*update_metadata=*/true, &error);
        if (unlikely(rc != 0))
        {
            dst->v = (struct ppb_field_value) { .ptr = NULL };
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

    /*@ assert limit_error < 0 ==> src.size ≥ break_size; */
    /*@ assert limit ≤ buf.size ==> break_size ≡ buf.size - limit; */
    /*@ assert limit > buf.size ==> break_size ≡ 0; */
    /*@ assert limit_error < 0 ==> consumed ≤ limit; */

    return consumed;
}

/*@ requires \valid(buf);
  @ requires buf_valid_range(*buf);
  @ requires buf->size ≤ (size_t)PTRDIFF_MAX;
  @ requires \valid_read(tags + (0..num_fields - 1));
  @ requires \valid(fields + (0..num_fields - 1));
  @ requires \separated(tags + (0..num_fields - 1), fields + (0..num_fields - 1));
  @ terminates \true;
  @ assigns g_initial_buf, *buf, fields[0..num_fields - 1];
  @ admit ensures lexn_buf_valid_after_copy:
  @    buf_valid(*buf);  // WP has trouble seeing through the final *buf = ...; copy
  @ ensures ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \old(fields[j].m);
  @ ensures \result.first_field + \result.field_range ≤ num_fields;
  @
  @ behavior well_formed:
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @  assumes limit_error ≤ 0;
  @ behavior hard_limit:
  @  // When a hard limit is in force and the call succeeds, bytes consumed ≤ limit.
  @  assumes ∀ integer j; 0 ≤ j < num_fields ==> tags[j].bits > 7;
  @  assumes limit_error < 0;
  @  ensures \result.status ≡ PPB_OK ==> \old(buf->size) - buf->size ≤ limit;
  @ behavior progress:
  @  // either consume at least one byte, or flag an error
  @  assumes buf->size > 0 ∧ max_lexed_fields > 0 ∧ limit > 0;
  @  ensures \result.status ≢ PPB_OK ∨ buf->size < \old(buf->size);
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
     * consumed >= limit.
     */
    const size_t break_size = saturating_subzu(src.size, limit);
    /*@ ghost size_t ghost_consumed = 0; */

    /*@ loop assigns i, fields[0..num_fields - 1], src, error, dummy, prev_tag, first_field, last_field,
      ghost_consumed;
      @ loop invariant 0 ≤ i ≤ max_lexed_fields;
      @ loop invariant buf_valid(src);
      @ loop invariant error ≡ PPB_OK;
      @ loop invariant prev_tag_monotonic: prev_tag ≥ \at(prev_tag, LoopCurrent) ≥ 7;
      @ loop invariant last_field ≡ 0 ∨ 0 ≤ last_field < num_fields;
      @ loop invariant num_fields ≡ 0 ==> first_field ≡ SIZE_MAX;
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

        dst->v.ptr = ptr;
        /*@ assert tag > 7; */
        int rc = handle_field(tag, dst, &src, false, &error);

        /*
         * handle_field with update_metadata=false ensures dst->m ≡ \old(dst->m),
         * (see nometa behavior), but the prover can't reason through the pointers.
         */
        /*@ admit lexn_meta_unchanged_in_loop:
          @    ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \at(fields[j].m, Pre);
          @*/

        if (unlikely(rc != 0))
        {
            dst->v = (struct ppb_field_value) { .ptr = NULL };
            /*@ admit lexn_meta_unchanged_in_loop_again:
              @    ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \at(fields[j].m, Pre);
              @*/
            break;
        }
    }

    /*@ assert ∀ integer j; 0 ≤ j < num_fields ==> fields[j].m ≡ \at(fields[j].m, Pre); @*/

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
    else
    {
        field_range = saturating_range_u32(first_field, last_field);
    }

    return (struct ppb_lexn_ret) {
        .first_field = first_field,
        .field_range = field_range,
        .status = error,
    };
}

#ifdef __FRAMAC__
#define PPB_EVA_BUF_SIZE   64
#define PPB_EVA_NUM_FIELDS 4

char eva_buf_storage[PPB_EVA_BUF_SIZE];
struct ppb_encoded_tag eva_tags[PPB_EVA_NUM_FIELDS];
struct ppb_field eva_fields[PPB_EVA_NUM_FIELDS];

/*@ terminates \true;
  @ assigns g_initial_buf, eva_fields[0..3];
  @*/
struct ppb_lexn_ret
ppb_entry_point(size_t buf_size, size_t limit, size_t num_fields, size_t max_lexed_fields)
{
    if (buf_size > PPB_EVA_BUF_SIZE)
        buf_size = PPB_EVA_BUF_SIZE;
    if (num_fields > PPB_EVA_NUM_FIELDS)
        num_fields = PPB_EVA_NUM_FIELDS;
    if (max_lexed_fields > PPB_EVA_NUM_FIELDS)
        max_lexed_fields = PPB_EVA_NUM_FIELDS;

    struct ppb_buf buf = { .buf = eva_buf_storage, .size = buf_size };

    enum ppb_error err = ppb_validate_tags(num_fields, eva_tags);
    if (err != PPB_OK)
        return (struct ppb_lexn_ret) { .status = err };

    /* prescan wrappers take buf by value, so reuse the same one. */
    ppb_prescan(buf, num_fields, eva_tags, eva_fields, max_lexed_fields);
    ppb_prescan_with_hard_limit(buf, limit, num_fields, eva_tags, eva_fields, max_lexed_fields);
    ppb_prescan_with_soft_limit(buf, limit, num_fields, eva_tags, eva_fields, max_lexed_fields);

    /* lexn wrappers mutate *buf; give each a fresh copy. */
    struct ppb_buf lex_buf = buf;
    ppb_lexn(&lex_buf, num_fields, eva_tags, eva_fields, max_lexed_fields);
    lex_buf = buf;
    ppb_lexn_with_hard_limit(&lex_buf, limit, num_fields, eva_tags, eva_fields, max_lexed_fields);
    lex_buf = buf;
    ppb_lexn_with_soft_limit(&lex_buf, limit, num_fields, eva_tags, eva_fields, max_lexed_fields);

    /* exercise the standalone helpers too. */
    enum ppb_error verr = PPB_OK;
    (void)ppb_decode_varint(&lex_buf, &verr);
    (void)ppb_zag(buf_size);
    (void)ppb_zag32(buf_size);
    (void)squish_varint_portable(12345);

    return (struct ppb_lexn_ret) { .status = verr };
}
#endif
