#pragma once

#include <ppb/ppb.hpp>

// clang-format off

// Fields dropped / constructs ignored by protoc-gen-ppb
// (not decoded; these fields/constructs are dropped from the schema):
//   ppb-dropped: .demo.Foo.g (group field (wire types 3/4 are undecodable))

namespace ppb_gen::demo::Foo
{
    enum class F : ::std::int32_t
    {
    };

    constexpr ::std::size_t max_depth = 0;

    /*
     * demo.Foo declares no fields, so this schema registers only the
     * unknown-field catch-all; a ppb::schema must hold at least one descriptor.
     *
     * If you only need to know whether Foo was present, do not pay to
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

namespace ppb_gen::demo::Foo::G
{
    enum class F : ::std::int32_t
    {
        a = 1,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        ::ppb::int32<F::a>>;

    using merge_schema = schema;
}

