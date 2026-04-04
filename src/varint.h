#pragma once
#include "ppb/ppb.h"

#include <limits.h>

#ifdef __x86_64__
#include <immintrin.h>

#ifndef __BMI2__
#warning "x86-64 build but BMI2 not enabled, this will be slower than it most likely should"
#endif
#endif

#include "buf.h"

/*
 * Removes the non-coding 8th bit in each byte of `x`.
 *
 * Assuming `x` masked off everything after the stop bit,
 * this is equivalent to decoding the varint.
 */
/*@ terminates \true;
  @ assigns \result \from x;
  @*/
static inline uint64_t
squish_varint(uint64_t x)
{
#ifdef __BMI2__
    return _pext_u64(x, 127UL * (UINT64_MAX / UINT8_MAX));
#else
    /* generate 4x pairs of 14 bits every 16 */
    {
        uint64_t mask = (127UL * (UINT64_MAX / UINT16_MAX));
        uint64_t lo = x & mask;
        /* Strip continuation bits while merging. */
        uint64_t hi = x & (~mask & 127UL * (UINT64_MAX / UINT8_MAX));

        x = lo | (hi >> 1);
    }

    /* 2x quads of 28 bits every 32. */
    {
        uint64_t mask = (UINT16_MAX * (UINT64_MAX / UINT32_MAX));
        uint64_t lo = x & mask;
        uint64_t hi = x & ~mask;

        x = lo | (hi >> 2);
    }

    /* final result */
    {
        uint64_t mask = UINT32_MAX;
        uint64_t lo = x & mask;
        uint64_t hi = x & ~mask;
        x = lo | (hi >> 4);
    }

    return x;
#endif
}

/*
 * Slow path of attempting to decode the varint at the head of `src`.
 *
 * Returns the strictly positive number of bytes read on success,
 * PPB_ERROR_TRUNCATED_DATA if we ran out of bytes while expecting
 * more varint bytes, and `corrupt_error` when the varint would be too
 * long to fit in a uint64_t (this is fewer than 64 bits of varint
 * bits for tags, but that's OK because tags *after adding the wire
 * type* should be 32-bit integers at most).
 *
 * This slow path always works, but is most useful when the varint is
 * close to the end of the buffer, or when the varint might be encoded
 * over more than 8 bytes (impossible for a tag).
 *
 * The `limb_width` determines how many bits we select from each byte.
 * This is 7 for a regular varint, but 8 to simulate the way we mask
 * off irrelevant bytes after a tag, without squishing them.
 */
/*@ requires buf_valid(src);
  @ requires limb_width ∈ {7, 8};
  @ requires (int)corrupt_error < 0;
  @ terminates \true;
  @ assigns *OUT_varint;
  @ ensures buf_valid(src);
  @ ensures \result ≢ 0;
  @ ensures \result < 0 ∨ (\result > 0 ∧ \result ≤ src.size);
  @ ensures \result < 0 ==> *OUT_varint ≡ 0;
  @*/
NOINLINE static int
peek_varint_slow(struct ppb_buf src, uint64_t *restrict OUT_varint, size_t limb_width,
    enum ppb_error corrupt_error)
{
    uint64_t ret = 0;
    int consumed_bytes = 0;
    uint64_t mask = (1U << limb_width) - 1;

    *OUT_varint = 0;

    /*@ loop assigns ret, shift, consumed_bytes;
      @ loop invariant shift ≤ 71;
      @ loop invariant 0 ≤ consumed_bytes ≤ shift;
      @ loop invariant *OUT_varint ≡ 0;
      @ loop variant 71 - shift;
      @*/
    for (size_t shift = 0; shift < 64; shift += limb_width)
    {
        if (unlikely((size_t)consumed_bytes >= src.size))
        {
            return PPB_ERROR_TRUNCATED_DATA;
        }

        uint64_t bits = (uint8_t)((const char *)src.buf)[consumed_bytes];
        consumed_bytes++;

        ret |= (bits & mask) << shift;
        if (!(bits & 128))
        {
            *OUT_varint = ret;
            return consumed_bytes;
        }
    }

    return corrupt_error;
}

/*
 * Attempts to decode the next varint at the head of `src`.
 *
 * Populates `OUT_varint` with the result (or 0 on error), and returns
 * the strictiy positive number of bytes read on success, or a
 * negative `enum ppb_error` value on failure.
 */
/*@ requires buf_valid(src);
  @ terminates \true;
  @ assigns *OUT_varint;
  @ ensures buf_valid(src);
  @ ensures \result < 0 ∨ (\result > 0 ∧ \result ≤ src.size);
  @ ensures \result < 0 ==> *OUT_varint ≡ 0;
  @*/
static inline int
peek_varint(struct ppb_buf src, uint64_t *restrict OUT_varint)
{
    /* optional fast path */
    if (likely(src.size >= sizeof(uint64_t)))  /* mutant-ok: '>=' -> '>' mutant-ok: 'if:force_false' */
    {
        uint64_t bits = buf_peek64(src);
        uint64_t high_bits = 128UL * (UINT64_MAX / UINT8_MAX); /* zero hits slow path mutant-ok: '*' -> '/' */
        uint64_t stop_bits = high_bits & ~bits; /* looking for the first zero (now set) continuation bit. */

        /* any `stop_bits` is zero in `bits`, so their value in `content_mask` is irrelevant. */
        uint64_t content_mask = stop_bits - 1;

        if (likely(stop_bits != 0)) /* falls through to slow path, mutant-ok: 'if:force_false' */
        {
            int count = 1 + (__builtin_ctzll(stop_bits) / 8);
            uint64_t ret = squish_varint(bits & content_mask);

            *OUT_varint = ret;
            return count; /* can always fall through to slow path, so mutant-ok: 'delete' */
        }
    }

    /* close to the end, or more than 8 bytes. */
    return peek_varint_slow(src, OUT_varint, /*limb_width=*/7, PPB_ERROR_CORRUPT_VARINT);
}

/*
 * Attemps to decode and consume the next varint at the head of `src`;
 * the `error` must be zero on entry.
 *
 * Returns the decoded value on success, or 0 on error (but 0 is also
 * a valid varint, so we must disambiguate by looking at `error`).
 *
 * The error is set to a strictly negative value when decoding failed.
 */
/*@ requires \valid(src);
  @ requires buf_valid(*src);
  @ requires \valid(error);
  @ requires *error ≡ 0;
  @ terminates \true;
  @ assigns *src, *error;
  @ ensures buf_valid(*src);
  @ ensures *error ≤ 0;
  @ ensures *error ≢ 0 ==> \result ≡ 0;
  @ ensures src->size ≤ \old(src->size);
  @ ensures src->buf ≡ (const char *)\old(src->buf) + \old(src->size) - src->size;
  @*/
static inline uint64_t
decode_varint(struct ppb_buf *restrict src, enum ppb_error *restrict error)
{
    uint64_t ret;
    int num = peek_varint(*src, &ret);

    /*@ assert num ≢ 0; */
    if (likely(num >= 0)) /* num != 0, mutant-ok: '>=' -> '>' */
    {
        buf_advance(src, num);
        return ret;
    }

    error_set(error, (enum ppb_error)num);
    return 0;
}

/*
 * Attempts to decode the next tag at the head of `src`.
 *
 * Populates `OUT_varint` with the result (or 0 on error), and returns
 * the strictiy positive number of bytes read on success, or a
 * negative `enum ppb_error` value on failure.
 *
 * Tag decoding differs from varint decoding only in that the
 * continuation/stop bit at the top of every byte is left in place;
 * this is fine for tag purposes because we only care about the low 3
 * bits (same regardless of the top bits), ordering (unaffected by the
 * top bits), and identity (fine as long as we the top bits match on
 * both sides).  Leaving the top bits avoids a call to `squish_varint`,
 * which is currently non-trivial outside x86.
 *
 * It's also fine to keep these redundant bits in because a valid protobuf
 * tag must fit in a uint32_t after inserting the 3 wire type bits: protobuf
 * field indices must be in [1, 2**29 - 1].
 */
/*@ requires buf_valid(src);
  @ terminates \true;
  @ assigns *OUT_tag;
  @ ensures \result ≢ 0;
  @ ensures \result < 0 ∨ (\result > 0 ∧ \result ≤ src.size);
  @ ensures \result < 0 ==> *OUT_tag ≡ 0;
  @*/
static inline int
peek_tag(struct ppb_buf src, uint64_t *restrict OUT_tag)
{
    *OUT_tag = 0;
    /* optional fast path */
    if (likely(src.size >= sizeof(uint64_t))) /* mutant-ok: '>=' -> '>' mutant-ok: 'if:force_false' */
    {
        uint64_t bits = buf_peek64(src);
        uint64_t high_bits = 128UL * (UINT64_MAX / UINT8_MAX);
        uint64_t stop_bits = high_bits & ~bits; /* looking for the first zero (now set) continuation bit. */

        /* any `stop_bits` is zero in `bits`, so their value in `content_mask` is irrelevant. */
        uint64_t content_mask = stop_bits - 1;

        if (unlikely(stop_bits == 0))
        {
            /* that's an invalid (overlong) tag. */
            return PPB_ERROR_CORRUPT_TAG;
        }

        int count = 1 + (__builtin_ctzll(stop_bits) / 8);
        uint64_t ret = bits & content_mask;

        *OUT_tag = ret;
        return count; /* can always fall through to slow path, so mutant-ok: 'delete' */
    }

    return peek_varint_slow(src, OUT_tag, /*limb_width=*/8, PPB_ERROR_CORRUPT_TAG);
}

/*
 * Attemps to decode and consume the next tag at the head of `src`;
 * the `error` must be zero on entry.
 *
 * Returns the decoded value on success, or 0 on error (but 0 is also
 * a valid varint, so we must disambiguate by looking at `error`).
 * See `peek_tag` for details on the tag encoding.
 *
 * The error is set to a strictly negative value when decoding failed.
 */
/*@ requires \valid(src);
  @ requires buf_valid(*src);
  @ requires \valid(error);
  @ requires *error ≡ 0;
  @ terminates \true;
  @ assigns *src, *error;
  @ ensures buf_valid(*src);
  @ ensures *error ≤ 0;
  @ ensures *error ≢ 0 ==> \result ≡ 0;
  @ ensures src->size ≤ \old(src->size);
  @ ensures src->buf ≡ (const char *)\old(src->buf) + \old(src->size) - src->size;
  @*/
static inline uint64_t
decode_tag(struct ppb_buf *restrict src, enum ppb_error *restrict error)
{
    uint64_t ret;
    int num = peek_tag(*src, &ret);

    /*@ assert num ≢ 0; */
    if (likely(num >= 0)) /* num != 0, mutant-ok: '>=' -> '>' */
    {
        buf_advance(src, num);
        return ret;
    }

    error_set(error, (enum ppb_error)num);
    return 0;
}
