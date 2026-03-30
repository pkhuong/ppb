#pragma once
#include <limits.h>
#include <stdint.h>

/*@ terminates \true;
  @ assigns \result \from a, mask;
  @ exits \false;
  @*/
uint64_t _pext_u64(uint64_t a, uint64_t mask);

/*@ requires x > 0;
  @ terminates \true;
  @ exits \false;
  @ assigns \result \from x;
  @ ensures 0 ≤ \result < CHAR_BIT * sizeof(unsigned long long);
  @*/
int __builtin_ctzll(unsigned long long x);
