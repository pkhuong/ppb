#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::std::Node
{
    enum class F : ::std::int32_t
    {
        v = 1,
        next = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        ::ppb::proto3_int32<F::v>,
        ::ppb::bytes<F::next, ::std::byte, ::ppb::field_semantics::singular> /* recursive: opaque */>;

    using merge_schema = ::ppb::auto_schema<
        ::ppb::int32<F::v>,
        ::ppb::bytes<F::next, ::std::byte, ::ppb::field_semantics::singular> /* recursive: opaque */>;
}

