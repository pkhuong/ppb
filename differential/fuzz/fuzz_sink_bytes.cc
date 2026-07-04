/*
 * Arbitrary-bytes differential fuzzer: full-mode PPB reflection
 * sink vs libprotobuf's parser.
 *
 * Raw fuzzer bytes are fed to both `ParsePartialFromString` and the
 * generated full-mode `parse_into` for a few schemas.  The two
 * parsers must agree on accept/reject, and, for accepted inputs, the
 * decoded messages must match field-by-field (including retained
 * unknown fields, since MessageDifferencer compares those too).
 *
 * Whitelisted divergences (everything else traps):
 *
 * - libprotobuf accepts, PPB accepts, values differ: OK iff PPB
 *   decodes libprotobuf's canonical re-serialization back to the
 *   reference (`ppb_agrees_on_canonical`).
 * - libprotobuf accepts, PPB rejects: OK iff libprotobuf parsed a
 *   group (wire type 3/4) somewhere (PPB rejects groups) or
 *   `ppb_agrees_on_canonical`, which means the rejection is on non-canonical
 *   encoding (known case: tag varints above uint32, which libprotobuf truncates
 *   while the sink rejects the un-truncated field number above 2^29-1).
 * - libprotobuf rejects, PPB accepts: OK if the decoded message [likely] has
 *   overlong framing varints that are canonicalized by PPB, or the decoded
 *   message once re-serialized to bytes is still rejected by libprotobuf (that
 *   means malformed structure survived decoding with PPB's looser validation,
 *   so it wasn't laundered into a different valid value)
 */
#include "pbcomposite3.pb.h"
#include "pbcomposite3.ppb.reflect.hpp"
#include "pbscalars3.pb.h"
#include "pbscalars3.ppb.reflect.hpp"

#include <cstddef>
#include <cstdint>

#define POSTCOND(cond)        \
    do                        \
    {                         \
        if (!(cond))          \
            __builtin_trap(); \
    } while (0)

#include "reflection_differential.hpp"

namespace
{

inline std::span<const std::byte>
as_bytes(const uint8_t *data, size_t size)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte *>(data), size);
}

}  // namespace

extern "C" int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const auto input = as_bytes(data, size);

    ppb_fuzz::differential_check<ppb_fuzz::LeniencyPolicy {}, demo_pb::Scalars>("Scalars", input,
        [](std::span<const std::byte> b, ppb_fuzz::Message *m)
        { return ppb_gen::demo_pb::Scalars::parse_into(b, m); });

    ppb_fuzz::differential_check<ppb_fuzz::LeniencyPolicy {}, demo_comp3::Maps>("Maps", input,
        [](std::span<const std::byte> b, ppb_fuzz::Message *m)
        { return ppb_gen::demo_comp3::Maps::parse_into(b, m); });

    ppb_fuzz::differential_check<ppb_fuzz::LeniencyPolicy {}, demo_comp3::Repeated>("Repeated", input,
        [](std::span<const std::byte> b, ppb_fuzz::Message *m)
        { return ppb_gen::demo_comp3::Repeated::parse_into(b, m); });

    return 0;
}
