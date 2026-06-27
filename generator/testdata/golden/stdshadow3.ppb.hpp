#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo::std::Foo
{
    enum class F : ::std::int32_t
    {
        x = 1,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        ::ppb::proto3_int32<F::x>>;

    using merge_schema = ::ppb::auto_schema<
        ::ppb::int32<F::x>>;
}

