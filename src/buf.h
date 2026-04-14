#pragma once
#include "cc.h"
#include "error.h"
#include "ppb/ppb.h"

#include <assert.h>

/*@ predicate buf_valid_range(struct ppb_buf buf) =
  @    \valid_read((const char *)buf.buf + (0..buf.size - 1));
  @*/

/* Internally, ppb_buf are a prefix of the externally provided g_initial_buf. */
/*@ ghost struct ppb_buf g_initial_buf; */

/*@ predicate buf_valid(struct ppb_buf buf) =
  @    buf_valid_range(g_initial_buf)
  @  ∧ buf.size ≤ g_initial_buf.size ≤ PTRDIFF_MAX
  @  ∧ (const char *)buf.buf ≡ ((const char *)g_initial_buf.buf + (g_initial_buf.size - buf.size))
  @  ∧ buf_valid_range(buf);
  @*/

/*
 * Checks whether there's room to read `delta` bytes in `src`.
 */
/*@ requires buf_valid(src);
  @ requires \valid(error);
  @ requires error_non_pos: *error ≤ 0;
  @ terminates \true;
  @ assigns *error;
  @ ensures error_non_pos: *error ≤ 0;
  @ ensures ok_is_in_range: \result ≡ 0 ==> delta ≤ src.size;
  @ ensures ok_is_readable: \result ≡ 0 ==> \valid_read((const char *)src.buf + (0.. delta - 1));
  @ behavior sticky_neg:
  @  assumes *error < 0;
  @  ensures *error ≡ \old(*error);
  @ behavior flag:
  @  assumes *error ≡ 0;
  @  ensures *error ≤ 0;
  @  ensures *error ≡ \result;
  @ complete behaviors sticky_neg, flag;
  @ disjoint behaviors sticky_neg, flag;
  @*/
static inline int
buf_check(struct ppb_buf src, uint64_t delta, enum ppb_error *restrict error)
{
    /*
     * delta can come from the wire, where we naturally decode to
     * uint64_t; try to be safe on 32-bit platforms, let someone else
     * figure > 64-bit platorms.
     */
    static_assert(sizeof(uint64_t) >= sizeof(size_t), "we assume size_t -> uint64_t is safe");

    if (unlikely(delta > src.size))
    {
        return error_set(error, PPB_ERROR_TRUNCATED_DATA);
    }

    return 0;
}

/*
 * Consumes `delts` bytes from `src.
 */
/*@ requires \valid(src);
  @ requires buf_valid(*src);
  @ requires delta ≤ src->size;
  @ terminates \true;
  @ assigns *src;
  @ ensures buf_valid(*src);
  @ ensures src->size ≡ \old(src->size) - delta;
  @ ensures src->buf ≡ (const char *)\old(src->buf) + delta;
  @ ensures src->buf ≡ (const char *)\old(src->buf) + \old(src->size) - src->size;
  @*/
static inline void
buf_advance(struct ppb_buf *restrict src, size_t delta)
{
    src->buf = (const char *)src->buf + delta;
    src->size -= delta;
    return;
}

/*
 * Decodes the little endian 64-bit value at head of `src`.
 */
/*@ requires buf_valid(src);
  @ requires src.size ≥ 8;
  @ terminates \true;
  @ assigns \nothing;
  @*/
static inline uint64_t
buf_peek64(const struct ppb_buf src)
{
    const char *bits_raw = src.buf;

    return (uint64_t)(unsigned char)bits_raw[0] | ((uint64_t)(unsigned char)bits_raw[1] << 8) |
        ((uint64_t)(unsigned char)bits_raw[2] << 16) | ((uint64_t)(unsigned char)bits_raw[3] << 24) |
        ((uint64_t)(unsigned char)bits_raw[4] << 32) | ((uint64_t)(unsigned char)bits_raw[5] << 40) |
        ((uint64_t)(unsigned char)bits_raw[6] << 48) | ((uint64_t)(unsigned char)bits_raw[7] << 56);
}

/*
 * Decodes the little endian 32-bit value at head of `src`.
 */
/*@ requires buf_valid(src);
  @ requires src.size ≥ 4;
  @ terminates \true;
  @ assigns \nothing;
  @*/
static inline uint64_t
buf_peek32(const struct ppb_buf src)
{
    const char *bits_raw = src.buf;

    return (uint64_t)(unsigned char)bits_raw[0] | ((uint64_t)(unsigned char)bits_raw[1] << 8) |
        ((uint64_t)(unsigned char)bits_raw[2] << 16) | ((uint64_t)(unsigned char)bits_raw[3] << 24);
}

/*
 * Decodes the 8-bit value at head of `src`.
 */
/*@ requires buf_valid(src);
  @ requires src.size ≥ 1;
  @ terminates \true;
  @ assigns \nothing;
  @*/
static inline uint64_t
buf_peek8(const struct ppb_buf src)
{
    const char *bits_raw = src.buf;

    return (uint64_t)(unsigned char)bits_raw[0];
}
