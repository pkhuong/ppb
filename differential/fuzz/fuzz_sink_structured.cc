/*
 * Structure-aware fuzzer for the full-mode libprotobuf reflection sink.
 *
 * libprotobuf-mutator generates *valid* typed messages; we serialize
 * them with libprotobuf, decode the wire bytes with the generated
 * `parse_into`, and trap unless the result matches libprotobuf's own
 * re-parse of the same bytes.  This explores deep/weird-but-valid
 * message space (nested maps, extreme varints, long repeated runs)
 * that the byte-level fuzzers only reach by luck.  Sanitizers
 * (ASan+UBSan) catch UB; POSTCOND traps on divergence.
 *
 * The oracle is libprotobuf's *parser*, not the mutated message
 * itself: LPM mutates map entries through reflection and can build
 * in-memory duplicates of one key, which serialize as two entries;
 * both parsers then agree on last-wins, but the in-memory original
 * still holds both.  NaN payloads also require treat_nan_as_equal.
 *
 * The full sink enforces proto3 UTF-8 (PPB_ERROR_INVALID_UTF8), so
 * when the reference parser rejects the serialized bytes the sink
 * must reject too.  A matching PPB rejection is the only quiet
 * outcome; PPB acceptance is a bug that traps.
 *
 * One target message type per binary, selected at compile time:
 *   -DPPB_STRUCT_TARGET_SCALARS   demo_pb.Scalars     (pbscalars3.proto)
 *   -DPPB_STRUCT_TARGET_MAPS      demo_comp3.Maps     (pbcomposite3.proto)
 *   -DPPB_STRUCT_TARGET_REPEATED  demo_comp3.Repeated (pbcomposite3.proto)
 *
 * Built and run inside the pinned container image (see differential/Dockerfile
 * / differential/fuzz/build_and_run.sh); libprotobuf-mutator is not packaged in
 * Debian or Ubuntu, so the image builds it from a pinned upstream tag.
 */
#if defined(PPB_STRUCT_TARGET_SCALARS)
#include "pbscalars3.pb.h"
#include "pbscalars3.ppb.reflect.hpp"
using fuzz_message = demo_pb::Scalars;
#define PPB_STRUCT_PARSE_INTO ::ppb_gen::demo_pb::Scalars::parse_into
#elif defined(PPB_STRUCT_TARGET_MAPS)
#include "pbcomposite3.pb.h"
#include "pbcomposite3.ppb.reflect.hpp"
using fuzz_message = demo_comp3::Maps;
#define PPB_STRUCT_PARSE_INTO ::ppb_gen::demo_comp3::Maps::parse_into
#elif defined(PPB_STRUCT_TARGET_REPEATED)
#include "pbcomposite3.pb.h"
#include "pbcomposite3.ppb.reflect.hpp"
using fuzz_message = demo_comp3::Repeated;
#define PPB_STRUCT_PARSE_INTO ::ppb_gen::demo_comp3::Repeated::parse_into
#else
#error "define one PPB_STRUCT_TARGET_{SCALARS,MAPS,REPEATED}"
#endif

#include <cstddef>
#include <cstdio>
#include <google/protobuf/util/field_comparator.h>
#include <google/protobuf/util/message_differencer.h>
#include <libprotobuf-mutator/src/libfuzzer/libfuzzer_macro.h>
#include <span>
#include <string>

#define POSTCOND(cond)        \
    do                        \
    {                         \
        if (!(cond))          \
            __builtin_trap(); \
    } while (0)

namespace
{

inline std::span<const std::byte>
as_bytes(const std::string &wire)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte *>(wire.data()), wire.size());
}

}  // namespace

DEFINE_PROTO_FUZZER(const fuzz_message &original)
{
    const std::string wire = original.SerializePartialAsString();

    fuzz_message reference;
    const bool reference_ok = reference.ParsePartialFromString(wire);

    fuzz_message decoded;
    const ppb_error err = PPB_STRUCT_PARSE_INTO(as_bytes(wire), &decoded);

    if (!reference_ok)
    {
        /*
         * libprotobuf rejected its own serializer's output.  The known
         * cause is proto3 UTF-8 enforcement, which the full sink also
         * enforces (PPB_ERROR_INVALID_UTF8).  PPB must therefore reject
         * too; acceptance means the sink let through bytes the
         * reference parser refuses, which is a bug.
         */
        if (err != PPB_OK)
            return;

        std::fprintf(stderr,
            "ppb accepted %zu wire bytes that libprotobuf rejects "
            "(sink UTF-8 validation gap)\ndecoded:\n%s\n",
            wire.size(), decoded.DebugString().c_str());
        POSTCOND(false);
    }

    if (err != PPB_OK)
    {
        std::fprintf(stderr, "parse_into failed: ppb_error %d on %zu wire bytes\n", static_cast<int>(err),
            wire.size());
        POSTCOND(false);
    }

    google::protobuf::util::MessageDifferencer diff;
    google::protobuf::util::DefaultFieldComparator cmp;

    cmp.set_treat_nan_as_equal(true);
    diff.set_field_comparator(&cmp);

    if (!diff.Compare(reference, decoded))
    {
        std::fprintf(stderr, "differential divergence (%zu wire bytes)\nreference:\n%s\ndecoded:\n%s\n",
            wire.size(), reference.DebugString().c_str(), decoded.DebugString().c_str());
        POSTCOND(false);
    }
}
