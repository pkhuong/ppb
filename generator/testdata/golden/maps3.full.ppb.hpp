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
        ::ppb::proto3_utf8string<F::key>,
        ::ppb::proto3_int32<F::value>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = ::ppb::auto_schema<
        ::ppb::utf8string<F::key>,
        ::ppb::int32<F::value>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;
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
        ::ppb::proto3_int64<F::key>,
        ::ppb::proto3_utf8string<F::value>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = ::ppb::auto_schema<
        ::ppb::int64<F::key>,
        ::ppb::utf8string<F::value>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;
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
        ::ppb::unpacked_message<F::counts, ::ppb_gen::demo_map::WithMap::CountsEntry::schema>,
        ::ppb::unpacked_message<F::names, ::ppb_gen::demo_map::WithMap::NamesEntry::schema>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = schema;
}

