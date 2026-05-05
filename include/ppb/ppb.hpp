#pragma once

#include "ppb.h"

#include <bit>
#include <cstddef>
#include <cstdint>

static_assert(std::endian::native == std::endian::little, "ppb.hpp currently requires a little-endian host");

namespace ppb
{

enum class wire_type : uint8_t
{
    varint = PPB_WIRE_VARINT,
    i64 = PPB_WIRE_I64,
    len = PPB_WIRE_LEN,
    i32 = PPB_WIRE_I32,
};

// PPB field descriptors all inherit from `field_base<K, T>`, which is-a
// `field_generic_base`.
struct field_generic_base;
template <auto K, wire_type type> struct field_base;

// A varint-encoded field
template <auto K> struct varint;
// An i64-encoded field
template <auto K> struct i64;
// A length-prefixed field
template <auto K> struct len;
// An i32-encoded field
template <auto K> struct i32;

}  // namespace ppb

// clang-format off
#include "ppb_detail.hpp"
// clang-format on

namespace ppb
{
}  // namespace ppb
