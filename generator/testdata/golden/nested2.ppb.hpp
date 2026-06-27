#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo2::Outer::Inner
{
    enum class F : ::std::int32_t
    {
        a = 1,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::a>(...)
        ::ppb::int32<F::a>>;

    using merge_schema = schema;
}

namespace ppb_gen::demo2::Outer
{
    enum class F : ::std::int32_t
    {
        inner = 1,
        unpacked_vals = 2,
        packed_vals = 3,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::inner, ::ppb_gen::demo2::Outer::Inner::merge_schema>(...)
        ::ppb::message<F::inner, ::ppb_gen::demo2::Outer::Inner::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_each<F::unpacked_vals>(...)
        ::ppb::unpacked_int32<F::unpacked_vals>,
        ::ppb::packed_int32<F::unpacked_vals, ::ppb::field_semantics::error>,
        // ppb::on_bulk<F::packed_vals>(range_fn, elem_fn)
        ::ppb::packed_int32<F::packed_vals>,
        ::ppb::int32<F::packed_vals, ::ppb::field_semantics::error>>;

    using merge_schema = schema;
}

