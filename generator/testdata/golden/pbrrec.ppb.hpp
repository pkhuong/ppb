#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_rrec::Tree
{
    enum class F : ::std::int32_t
    {
        value = 1,
        children = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::value>(...)
        ::ppb::proto3_int32<F::value>,
        // ppb::on_submessage<F::children, ::ppb_gen::demo_rrec::Tree::schema>(...)
        ::ppb::unpacked_bytes<F::children> /* recursive: opaque */>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::value>(...)
        ::ppb::int32<F::value>,
        // ppb::on_submessage<F::children, ::ppb_gen::demo_rrec::Tree::schema>(...)
        ::ppb::unpacked_bytes<F::children> /* recursive: opaque */>;
}

