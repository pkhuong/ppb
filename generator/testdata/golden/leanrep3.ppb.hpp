#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_lean
{
    enum class Color : ::std::int32_t
    {
        UNSET = 0,
        RED = 1,
    };
}

namespace ppb_gen::demo_lean::Rep
{
    enum class F : ::std::int32_t
    {
        vals = 1,
        colors = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        ::ppb::packed_int32<F::vals>,
        ::ppb::int32<F::vals, ::ppb::field_semantics::error>,
        ::ppb::packed_enumerated<F::colors, ::ppb_gen::demo_lean::Color>,
        ::ppb::enumerated<F::colors, ::ppb_gen::demo_lean::Color, ::ppb::field_semantics::error>>;

    using merge_schema = schema;
}

