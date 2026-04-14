#pragma once
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/*@ terminates \true;
  @ exits \false;
  @ assigns \result \from x, y;
  @ behavior saturate:
  @  assumes x < y;
  @  ensures \result ≡ 0;
  @ behavior normal:
  @  assumes x ≥ y;
  @  ensures \result ≡ x - y;
  @ disjoint behaviors saturate, normal;
  @ complete behaviors saturate, normal;
  @*/
static inline size_t
saturating_subzu(size_t x, size_t y)
{
    size_t diff;
    return __builtin_sub_overflow(x, y, &diff) ? 0 : diff;
}

/*@ requires x ≤ y;
  @ terminates \true;
  @ assigns \result \from x, y;
  @ behavior in_range:
  @  assumes x ≤ y < x + UINT32_MAX;
  @  ensures \result ≡ 1 + y - x;
  @ behavior saturate:
  @  assumes y ≥ x + UINT32_MAX;
  @  ensures \result ≡ UINT32_MAX ≤ 1 + y - x;
  @ disjoint behaviors in_range, saturate;
  @ complete behaviors in_range, saturate;
  @*/
static inline uint32_t
saturating_range_u32(size_t x, size_t y)
{
    size_t result = y - x >= UINT32_MAX ? UINT32_MAX : (1 + y - x);
    /*@ assert 0 ≤ result ≤ UINT32_MAX; */
    return (uint32_t)result;
}
