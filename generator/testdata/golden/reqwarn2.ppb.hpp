#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::reqwarn2::Foo
{
    enum class F : ::std::int32_t
    {
        bar = 1,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::bar>(...)
        ::ppb::int32<F::bar>>;

    using merge_schema = schema;
}

