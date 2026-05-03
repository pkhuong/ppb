#pragma once

#include "ppb.h"

#include <bit>
#include <cstddef>
#include <cstdint>

static_assert(std::endian::native == std::endian::little, "ppb.hpp currently requires a little-endian host");

namespace ppb
{
}  // namespace ppb

// clang-format off
#include "ppb_detail.hpp"
// clang-format on

namespace ppb
{
}  // namespace ppb
