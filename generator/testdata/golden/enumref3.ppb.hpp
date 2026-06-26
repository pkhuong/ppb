#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_enumref::Defs
{
    enum class Color : ::std::int32_t
    {
        UNSET = 0,
        RED = 1,
    };
}

namespace ppb_gen::demo_enumref::Uses
{
    enum class F : ::std::int32_t
    {
        color = 1,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        ::ppb::proto3_enumerated<F::color, ::ppb_gen::demo_enumref::Defs::Color>>;

    using merge_schema = ::ppb::auto_schema<
        ::ppb::enumerated<F::color, ::ppb_gen::demo_enumref::Defs::Color>>;
}

namespace ppb_gen::demo_enumref::Defs
{
    enum class F : ::std::int32_t
    {
        v = 1,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        ::ppb::proto3_int32<F::v>>;

    using merge_schema = ::ppb::auto_schema<
        ::ppb::int32<F::v>>;
}

