#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo
{
    enum class Color : ::std::int32_t
    {
        RED = 0,
        GREEN = 1,
        BLUE = 2,
    };
}

namespace ppb_gen::demo::Scalars
{
    enum class F : ::std::int32_t
    {
        i = 1,
        u = 2,
        d = 3,
        s = 4,
        b = 5,
        c = 6,
        maybe = 7,
        packed_vals = 8,
        names = 9,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::i>(...)
        ::ppb::proto3_int32<F::i>,
        // ppb::on<F::u>(...)
        ::ppb::proto3_uint64<F::u>,
        // ppb::on<F::d>(...)
        ::ppb::proto3_f64<F::d>,
        // ppb::on<F::s>(...)
        ::ppb::proto3_utf8string<F::s>,
        // ppb::on<F::b>(...)
        ::ppb::proto3_bytes<F::b>,
        // ppb::on<F::c>(...)
        ::ppb::proto3_enumerated<F::c, ::ppb_gen::demo::Color>,
        // ppb::on<F::maybe>(...)
        ::ppb::int32<F::maybe>,
        // ppb::on_bulk<F::packed_vals>(range_fn, elem_fn)
        ::ppb::packed_int32<F::packed_vals>,
        ::ppb::int32<F::packed_vals, ::ppb::field_semantics::error>,
        // ppb::on_each<F::names>(...)
        ::ppb::unpacked_utf8string<F::names>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::i>(...)
        ::ppb::int32<F::i>,
        // ppb::on<F::u>(...)
        ::ppb::uint64<F::u>,
        // ppb::on<F::d>(...)
        ::ppb::f64<F::d>,
        // ppb::on<F::s>(...)
        ::ppb::utf8string<F::s>,
        // ppb::on<F::b>(...)
        ::ppb::bytes<F::b>,
        // ppb::on<F::c>(...)
        ::ppb::enumerated<F::c, ::ppb_gen::demo::Color>,
        // ppb::on<F::maybe>(...)
        ::ppb::int32<F::maybe>,
        // ppb::on_bulk<F::packed_vals>(range_fn, elem_fn)
        ::ppb::packed_int32<F::packed_vals>,
        ::ppb::int32<F::packed_vals, ::ppb::field_semantics::error>,
        // ppb::on_each<F::names>(...)
        ::ppb::unpacked_utf8string<F::names>>;
}

