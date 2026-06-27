#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_mrec::B
{
    enum class F : ::std::int32_t
    {
        bv = 1,
        a = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        ::ppb::proto3_int32<F::bv>,
        ::ppb::bytes<F::a, ::std::byte, ::ppb::field_semantics::singular> /* recursive: opaque */>;

    using merge_schema = ::ppb::auto_schema<
        ::ppb::int32<F::bv>,
        ::ppb::bytes<F::a, ::std::byte, ::ppb::field_semantics::singular> /* recursive: opaque */>;
}

namespace ppb_gen::demo_mrec::A
{
    enum class F : ::std::int32_t
    {
        av = 1,
        b = 2,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        ::ppb::proto3_int32<F::av>,
        ::ppb::message<F::b, ::ppb_gen::demo_mrec::B::merge_schema, ::ppb::field_semantics::singular>>;

    using merge_schema = ::ppb::auto_schema<
        ::ppb::int32<F::av>,
        ::ppb::message<F::b, ::ppb_gen::demo_mrec::B::merge_schema, ::ppb::field_semantics::singular>>;
}

