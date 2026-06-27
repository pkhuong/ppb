#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_empty::Empty
{
    enum class F : ::std::int32_t
    {
    };

    constexpr ::std::size_t max_depth = 0;

    /*
     * demo_empty.Empty declares no fields, so this schema registers only the
     * unknown-field catch-all; a ppb::schema must hold at least one descriptor.
     *
     * If you only need to know whether Empty was present, do not pay to
     * handle unknown fields here.  In the CONTAINING message, register
     * ppb::on<F::that_field>(...) to receive this submessage's raw span: that is
     * presence/absence detection without descending into it.  Handle unknown
     * fields here only when you must inspect the (otherwise unknown) contents.
     */
    using schema = ::ppb::auto_schema<
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = schema;
}

namespace ppb_gen::demo_empty::HasEmpty
{
    enum class F : ::std::int32_t
    {
        e = 1,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        ::ppb::message<F::e, ::ppb_gen::demo_empty::Empty::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = schema;
}

