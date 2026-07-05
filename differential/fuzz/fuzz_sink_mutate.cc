/*
 * Structured + byte-corruption differential fuzzer for the full-mode
 * libprotobuf reflection sink.  LPM generates a wrapper case (typed
 * target message + a list of byte-ops); the harness serializes the
 * target, applies the ops with the compile-time corruption variant, and
 * runs the policy-parameterized differential oracle against
 * libprotobuf's re-parse of the corrupted bytes.
 *
 *   target  -DPPB_MUT_TARGET_{SCALARS,MAPS,REPEATED}
 *   variant -DPPB_MUT_VARIANT_{ANY,NO_OVERLONG,ONE_TAG}
 */
#if defined(PPB_MUT_TARGET_SCALARS)
#include "fuzz_mutation.pb.h"
#include "pbscalars3.ppb.reflect.hpp"
using case_message = ppb_mut::ScalarsCase;
using target_message = demo_pb::Scalars;
#define PPB_MUT_PARSE_INTO ::ppb_gen::demo_pb::Scalars::parse_into
#define PPB_MUT_LABEL      "scalars"
#elif defined(PPB_MUT_TARGET_MAPS)
#include "fuzz_mutation.pb.h"
#include "pbcomposite3.ppb.reflect.hpp"
using case_message = ppb_mut::MapsCase;
using target_message = demo_comp3::Maps;
#define PPB_MUT_PARSE_INTO ::ppb_gen::demo_comp3::Maps::parse_into
#define PPB_MUT_LABEL      "maps"
#elif defined(PPB_MUT_TARGET_REPEATED)
#include "fuzz_mutation.pb.h"
#include "pbcomposite3.ppb.reflect.hpp"
using case_message = ppb_mut::RepeatedCase;
using target_message = demo_comp3::Repeated;
#define PPB_MUT_PARSE_INTO ::ppb_gen::demo_comp3::Repeated::parse_into
#define PPB_MUT_LABEL      "repeated"
#else
#error "define one PPB_MUT_TARGET_{SCALARS,MAPS,REPEATED}"
#endif

#include "corruption_engine.hpp"
#include "reflection_differential.hpp"

#include <libprotobuf-mutator/src/libfuzzer/libfuzzer_macro.h>
#include <span>
#include <string>
#include <vector>

#if defined(PPB_MUT_VARIANT_ANY)
inline constexpr ppb_fuzz::CorruptionVariant kVariant = ppb_fuzz::CorruptionVariant::ANY;
inline constexpr ppb_fuzz::LeniencyPolicy kPolicy {};  /* all tolerant */
#elif defined(PPB_MUT_VARIANT_NO_OVERLONG)
inline constexpr ppb_fuzz::CorruptionVariant kVariant = ppb_fuzz::CorruptionVariant::NO_OVERLONG;
inline constexpr ppb_fuzz::LeniencyPolicy kPolicy {
    /*tolerate_overlong_framing=*/false,
    /*tolerate_reparse_survival=*/true,
};
#elif defined(PPB_MUT_VARIANT_ONE_TAG)
inline constexpr ppb_fuzz::CorruptionVariant kVariant = ppb_fuzz::CorruptionVariant::ONE_TAG;
inline constexpr ppb_fuzz::LeniencyPolicy kPolicy {};  /* standard oracle */
#else
#error "define one PPB_MUT_VARIANT_{ANY,NO_OVERLONG,ONE_TAG}"
#endif

namespace
{

std::span<const std::byte>
as_bytes(const std::string &s)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte *>(s.data()), s.size());
}

std::vector<ppb_fuzz::CorruptOpPlain>
to_plain(const case_message &tc)
{
    std::vector<ppb_fuzz::CorruptOpPlain> ops;
    ops.reserve(static_cast<std::size_t>(tc.ops_size()));

    for (const ppb_mut::MutOp &op : tc.ops())
    {
        ops.push_back(ppb_fuzz::CorruptOpPlain {
            .kind = static_cast<ppb_fuzz::CorruptOpPlain::Kind>(op.kind()),
            .position = op.position(),
            .value = static_cast<std::uint8_t>(op.value() & 0xff),
        });
    }

    return ops;
}

}  // namespace

DEFINE_PROTO_FUZZER(const case_message &tc)
{
    const std::string wire = tc.msg().SerializePartialAsString();
    const std::vector<ppb_fuzz::CorruptOpPlain> ops = to_plain(tc);
    const std::string corrupted = ppb_fuzz::apply_corruptions(wire, ops, kVariant);

    ppb_fuzz::differential_check<kPolicy, target_message>(PPB_MUT_LABEL, as_bytes(corrupted),
        [](std::span<const std::byte> b, ppb_fuzz::Message *m) { return PPB_MUT_PARSE_INTO(b, m); });
}
