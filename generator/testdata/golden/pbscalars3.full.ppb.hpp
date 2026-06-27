#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_pb::Scalars
{
    enum class F : ::std::int32_t
    {
        i = 1,
        l = 2,
        u = 3,
        t = 4,
        z = 5,
        f = 6,
        g = 7,
        fl = 8,
        d = 9,
        b = 10,
        s = 11,
        by = 12,
        maybe = 13,
        packed_vals = 14,
        names = 15,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::i>(...)
        ::ppb::proto3_int32<F::i>,
        // ppb::on<F::l>(...)
        ::ppb::proto3_int64<F::l>,
        // ppb::on<F::u>(...)
        ::ppb::proto3_uint32<F::u>,
        // ppb::on<F::t>(...)
        ::ppb::proto3_uint64<F::t>,
        // ppb::on<F::z>(...)
        ::ppb::proto3_sint32<F::z>,
        // ppb::on<F::f>(...)
        ::ppb::proto3_fixed32<F::f>,
        // ppb::on<F::g>(...)
        ::ppb::proto3_sfixed64<F::g>,
        // ppb::on<F::fl>(...)
        ::ppb::proto3_f32<F::fl>,
        // ppb::on<F::d>(...)
        ::ppb::proto3_f64<F::d>,
        // ppb::on<F::b>(...)
        ::ppb::proto3_boolean<F::b>,
        // ppb::on<F::s>(...)
        ::ppb::proto3_utf8string<F::s>,
        // ppb::on<F::by>(...)
        ::ppb::proto3_bytes<F::by>,
        // ppb::on<F::maybe>(...)
        ::ppb::int32<F::maybe>,
        // ppb::on_bulk<F::packed_vals>(range_fn, elem_fn)
        ::ppb::packed_int32<F::packed_vals>,
        ::ppb::int32<F::packed_vals, ::ppb::field_semantics::always_lexn>,
        // ppb::on_each<F::names>(...)
        ::ppb::unpacked_utf8string<F::names>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::i>(...)
        ::ppb::int32<F::i>,
        // ppb::on<F::l>(...)
        ::ppb::int64<F::l>,
        // ppb::on<F::u>(...)
        ::ppb::uint32<F::u>,
        // ppb::on<F::t>(...)
        ::ppb::uint64<F::t>,
        // ppb::on<F::z>(...)
        ::ppb::sint32<F::z>,
        // ppb::on<F::f>(...)
        ::ppb::fixed32<F::f>,
        // ppb::on<F::g>(...)
        ::ppb::sfixed64<F::g>,
        // ppb::on<F::fl>(...)
        ::ppb::f32<F::fl>,
        // ppb::on<F::d>(...)
        ::ppb::f64<F::d>,
        // ppb::on<F::b>(...)
        ::ppb::boolean<F::b>,
        // ppb::on<F::s>(...)
        ::ppb::utf8string<F::s>,
        // ppb::on<F::by>(...)
        ::ppb::bytes<F::by>,
        // ppb::on<F::maybe>(...)
        ::ppb::int32<F::maybe>,
        // ppb::on_bulk<F::packed_vals>(range_fn, elem_fn)
        ::ppb::packed_int32<F::packed_vals>,
        ::ppb::int32<F::packed_vals, ::ppb::field_semantics::always_lexn>,
        // ppb::on_each<F::names>(...)
        ::ppb::unpacked_utf8string<F::names>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;
}

