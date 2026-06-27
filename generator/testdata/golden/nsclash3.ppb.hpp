#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::a::b::a
{
    enum class F : ::std::int32_t
    {
        x = 1,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::x>(...)
        ::ppb::proto3_int32<F::x>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::x>(...)
        ::ppb::int32<F::x>>;
}

namespace ppb_gen::a::b::Outer
{
    enum class F : ::std::int32_t
    {
        ref = 1,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::ref, ::ppb_gen::a::b::a::merge_schema>(...)
        ::ppb::message<F::ref, ::ppb_gen::a::b::a::merge_schema, ::ppb::field_semantics::singular>>;

    using merge_schema = schema;
}

