#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_map::WithMap::CountsEntry
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

namespace ppb_gen::demo_map::WithMap::NamesEntry
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

namespace ppb_gen::demo_map::WithMap
{
    enum class F : ::std::int32_t
    {
        counts = 1,
        names = 2,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::counts, ::ppb_gen::demo_map::WithMap::CountsEntry::schema>(...)
        ::ppb::unpacked_message<F::counts, ::ppb_gen::demo_map::WithMap::CountsEntry::schema>,
        // ppb::on_submessage<F::names, ::ppb_gen::demo_map::WithMap::NamesEntry::schema>(...)
        ::ppb::unpacked_message<F::names, ::ppb_gen::demo_map::WithMap::NamesEntry::schema>>;

    using merge_schema = schema;
}

