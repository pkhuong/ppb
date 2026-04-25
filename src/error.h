#pragma once
#include "ppb/ppb.h"

/*
 * Sets `*error` to `update`, unless `*error` is already set (not OK).
 */
/*@ requires \valid(error);
  @ terminates \true;
  @ assigns *error \from *error, update;
  @ ensures \result ≡ update;
  @
  @ behavior well_formed:
  @  assumes *error ≤ 0 ∧ update ≤ 0;
  @  ensures *error ≤ 0;
  @
  @ behavior real_error:
  @  assumes *error ≤ 0;
  @  assumes update < 0;
  @  ensures error_is_set: *error < 0;
  @ behavior no_error:
  @  assumes update ≡ 0;
  @  ensures error_unchanged: *error ≡ \old(*error);
  @
  @ behavior initial:
  @  assumes *error ≡ 0;
  @  assumes update ≤ 0;
  @  ensures *error ≡ update ≤ 0;
  @ behavior sticky:
  @  assumes *error < 0;
  @  ensures *error ≡ \old(*error) < 0;
  @ disjoint behaviors initial, sticky;
  @*/
static inline int
error_set(enum ppb_error *error, enum ppb_error update)
{
    enum ppb_error initial = *error;
    *error = (initial == PPB_OK) ? update : initial;

    return (int)update;
}
