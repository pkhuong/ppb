#pragma once

#include <ppb/ppb.hpp>

// clang-format off

// Fields dropped / constructs ignored by protoc-gen-ppb
// (not decoded; these fields/constructs are dropped from the schema):
//   ppb-dropped: .demo2.WithStruct.payload (references well-known type .google.protobuf.Struct)

namespace ppb_gen::demo2::WithStruct
{
    enum class F : ::std::int32_t
    {
        id = 1,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::id>(...)
        ::ppb::int32<F::id>>;

    using merge_schema = schema;
}

