#pragma once
#include "ppb/ppb.h"

/*
 * Sets `*error` to `update`, unless `*error` is already set (not OK).
 */
/*@ requires \valid(error);
  @ requires nonpos: *error ≤ 0;
  @ requires nonpos_update: update ≤ 0;
  @ terminates \true;
  @ assigns *error \from *error, update;
  @ ensures \result ≡ (int)update;
  @
  @ behavior real_error:
  @  assumes update < 0;
  @  ensures error_is_set: *error < 0;
  @ behavior no_error:
  @  assumes update ≡ 0;
  @  ensures error_is_set: *error ≡ \old(*error);
  @
  @ behavior initial:
  @   assumes *error ≡ 0;
  @   ensures *error ≡ update ≤ 0;
  @ behavior sticky:
  @   assumes *error ≢ 0;
  @   ensures *error ≡ \old(*error) < 0;
  @ complete behaviors initial, sticky;
  @ disjoint behaviors initial, sticky;
  @*/
static inline int
error_set(enum ppb_error *error, enum ppb_error update)
{
    enum ppb_error initial = *error;
    *error = (initial == PPB_OK) ? update : initial;

    return (int)update;
}
