#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_comp3
{
    enum class Color : ::std::int32_t
    {
        UNSET = 0,
        RED = 1,
        GREEN = 2,
    };
}

namespace ppb_gen::demo_comp3::Enums
{
    enum class F : ::std::int32_t
    {
        c = 1,
        cs = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::c>(...)
        ::ppb::proto3_enumerated<F::c, ::ppb_gen::demo_comp3::Color>,
        // ppb::on_bulk<F::cs>(range_fn, elem_fn)
        ::ppb::packed_enumerated<F::cs, ::ppb_gen::demo_comp3::Color>,
        ::ppb::enumerated<F::cs, ::ppb_gen::demo_comp3::Color, ::ppb::field_semantics::error>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::c>(...)
        ::ppb::enumerated<F::c, ::ppb_gen::demo_comp3::Color>,
        // ppb::on_bulk<F::cs>(range_fn, elem_fn)
        ::ppb::packed_enumerated<F::cs, ::ppb_gen::demo_comp3::Color>,
        ::ppb::enumerated<F::cs, ::ppb_gen::demo_comp3::Color, ::ppb::field_semantics::error>>;
}

namespace ppb_gen::demo_comp3::Fixeds
{
    enum class F : ::std::int32_t
    {
        f32s = 1,
        s64s = 2,
        fls = 3,
        ds = 4,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on_bulk<F::f32s>(range_fn, elem_fn)
        ::ppb::packed_fixed32<F::f32s>,
        ::ppb::fixed32<F::f32s, ::ppb::field_semantics::error>,
        // ppb::on_bulk<F::s64s>(range_fn, elem_fn)
        ::ppb::packed_sfixed64<F::s64s>,
        ::ppb::sfixed64<F::s64s, ::ppb::field_semantics::error>,
        // ppb::on_bulk<F::fls>(range_fn, elem_fn)
        ::ppb::packed_f32<F::fls>,
        ::ppb::f32<F::fls, ::ppb::field_semantics::error>,
        // ppb::on_bulk<F::ds>(range_fn, elem_fn)
        ::ppb::packed_f64<F::ds>,
        ::ppb::f64<F::ds, ::ppb::field_semantics::error>>;

    using merge_schema = schema;
}

namespace ppb_gen::demo_comp3::Item
{
    enum class F : ::std::int32_t
    {
        id = 1,
        label = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::id>(...)
        ::ppb::proto3_int32<F::id>,
        // ppb::on<F::label>(...)
        ::ppb::proto3_utf8string<F::label>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::id>(...)
        ::ppb::int32<F::id>,
        // ppb::on<F::label>(...)
        ::ppb::utf8string<F::label>>;
}

namespace ppb_gen::demo_comp3::Repeated
{
    enum class F : ::std::int32_t
    {
        items = 1,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::items, ::ppb_gen::demo_comp3::Item::schema>(...)
        ::ppb::unpacked_message<F::items, ::ppb_gen::demo_comp3::Item::schema>>;

    using merge_schema = schema;
}

namespace ppb_gen::demo_comp3::Maps::CountsEntry
{
    enum class F : ::std::int32_t
    {
        key = 1,
        value = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::key>(...)
        ::ppb::proto3_utf8string<F::key>,
        // ppb::on<F::value>(...)
        ::ppb::proto3_int32<F::value>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::key>(...)
        ::ppb::utf8string<F::key>,
        // ppb::on<F::value>(...)
        ::ppb::int32<F::value>>;
}

namespace ppb_gen::demo_comp3::Maps::NamesEntry
{
    enum class F : ::std::int32_t
    {
        key = 1,
        value = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::key>(...)
        ::ppb::proto3_int64<F::key>,
        // ppb::on<F::value>(...)
        ::ppb::proto3_utf8string<F::value>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::key>(...)
        ::ppb::int64<F::key>,
        // ppb::on<F::value>(...)
        ::ppb::utf8string<F::value>>;
}

namespace ppb_gen::demo_comp3::Maps
{
    enum class F : ::std::int32_t
    {
        counts = 1,
        names = 2,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::counts, ::ppb_gen::demo_comp3::Maps::CountsEntry::schema>(...)
        ::ppb::unpacked_message<F::counts, ::ppb_gen::demo_comp3::Maps::CountsEntry::schema>,
        // ppb::on_submessage<F::names, ::ppb_gen::demo_comp3::Maps::NamesEntry::schema>(...)
        ::ppb::unpacked_message<F::names, ::ppb_gen::demo_comp3::Maps::NamesEntry::schema>>;

    using merge_schema = schema;
}

