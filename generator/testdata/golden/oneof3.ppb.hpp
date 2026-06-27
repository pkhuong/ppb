#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::pkg
{
    enum class Color : ::std::int32_t
    {
        RED = 0,
        GREEN = 1,
    };
}

namespace ppb_gen::pkg::Inner
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

namespace ppb_gen::pkg::HasOneof
{
    enum class F : ::std::int32_t
    {
        num = 1,
        sub = 2,
        col = 3,
        text = 4,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        ::ppb::int32<F::num, ::ppb::field_semantics::always_lexn>,
        ::ppb::message<F::sub, ::ppb_gen::pkg::Inner::merge_schema, ::ppb::field_semantics::always_lexn>,
        ::ppb::enumerated<F::col, ::ppb_gen::pkg::Color, ::ppb::field_semantics::always_lexn>,
        ::ppb::utf8string<F::text, ::ppb::field_semantics::always_lexn>>;

    using merge_schema = schema;
}

