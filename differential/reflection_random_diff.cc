/*
 * Randomized round-trip differential for the reflection sink.
 *
 * The fixed driver reflection_diff.cc checks hand-picked values,
 * while this one builds *random* messages through libprotobuf
 * reflection (deterministic seed), serializes them, decodes the bytes
 * with the generated `parse_into`, and asserts field-by-field
 * equality with MessageDifferencer.  Same build inputs and schema
 * modes as the fixed driver.
 *
 * Scope is deliberately limited, to steer clear of known divergences:
 *   - enum fields only take declared values;
 *   - map keys are de-duplicated by construction (MessageDifferencer is naive);
 *   - recursion depth stays far below the sink's default budget of 100.
 */
#include "empty3.pb.h"
#include "empty3.ppb.reflect.hpp"
#include "nested2.pb.h"
#include "nested2.ppb.reflect.hpp"
#include "oneof3.pb.h"
#include "oneof3.ppb.reflect.hpp"
#include "pbcomposite2.pb.h"
#include "pbcomposite2.ppb.reflect.hpp"
#include "pbcomposite3.pb.h"
#include "pbcomposite3.ppb.reflect.hpp"
#include "pbmrec.pb.h"
#include "pbmrec.ppb.reflect.hpp"
#include "pbrecursive.pb.h"
#include "pbrecursive.ppb.reflect.hpp"
#include "pbrrec.pb.h"
#include "pbrrec.ppb.reflect.hpp"
#include "pbscalars3.pb.h"
#include "pbscalars3.ppb.reflect.hpp"
#include "wkt_user3.pb.h"
#include "wkt_user3.ppb.reflect.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/message_differencer.h>
#include <random>
#include <span>
#include <string>

namespace
{

using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::Message;
using ::google::protobuf::Reflection;
using ::google::protobuf::util::MessageDifferencer;

struct rng
{
    std::minstd_rand engine;

    explicit rng(uint64_t seed)
        : engine(static_cast<std::minstd_rand::result_type>(seed))
    {
    }

    uint64_t next() { return engine(); }

    // Uniform draw in [0, n).
    uint64_t below(uint64_t n)
    {
        std::uniform_int_distribution<uint64_t> dist(0, n - 1);
        return dist(engine);
    }
};

std::string
random_ascii(rng &r)
{
    std::string out(r.below(12), '\0');
    for (char &c : out)
        c = static_cast<char>('a' + r.below(26));

    return out;
}

// Populate one (possibly repeated) field with random contents.
void randomize(Message *msg, rng &r, int depth);

void
random_field_value(Message *msg, const Reflection *refl, const FieldDescriptor *fd, rng &r, int depth)
{
    const bool rep = fd->is_repeated();

    switch (fd->cpp_type())
    {
    case FieldDescriptor::CPPTYPE_INT32:
    {
        auto v = static_cast<int32_t>(r.next());
        rep ? refl->AddInt32(msg, fd, v) : refl->SetInt32(msg, fd, v);
        break;
    }

    case FieldDescriptor::CPPTYPE_INT64:
    {
        auto v = static_cast<int64_t>(r.next());
        rep ? refl->AddInt64(msg, fd, v) : refl->SetInt64(msg, fd, v);
        break;
    }

    case FieldDescriptor::CPPTYPE_UINT32:
    {
        auto v = static_cast<uint32_t>(r.next());
        rep ? refl->AddUInt32(msg, fd, v) : refl->SetUInt32(msg, fd, v);
        break;
    }

    case FieldDescriptor::CPPTYPE_UINT64:
    {
        uint64_t v = r.next();
        rep ? refl->AddUInt64(msg, fd, v) : refl->SetUInt64(msg, fd, v);
        break;
    }

    case FieldDescriptor::CPPTYPE_DOUBLE:
    {
        auto v = static_cast<double>(static_cast<int64_t>(r.next())) / 3.0;
        rep ? refl->AddDouble(msg, fd, v) : refl->SetDouble(msg, fd, v);
        break;
    }

    case FieldDescriptor::CPPTYPE_FLOAT:
    {
        auto v = static_cast<float>(static_cast<int32_t>(r.next())) / 3.0f;
        rep ? refl->AddFloat(msg, fd, v) : refl->SetFloat(msg, fd, v);
        break;
    }

    case FieldDescriptor::CPPTYPE_BOOL:
    {
        bool v = (r.next() & 1) != 0;
        rep ? refl->AddBool(msg, fd, v) : refl->SetBool(msg, fd, v);
        break;
    }

    case FieldDescriptor::CPPTYPE_ENUM:
    {
        const auto *et = fd->enum_type();
        const auto *ev = et->value(static_cast<int>(r.below(uint64_t(et->value_count()))));
        rep ? refl->AddEnum(msg, fd, ev) : refl->SetEnum(msg, fd, ev);
        break;
    }

    case FieldDescriptor::CPPTYPE_STRING:
    {
        std::string v = random_ascii(r);
        rep ? refl->AddString(msg, fd, v) : refl->SetString(msg, fd, v);
        break;
    }

    case FieldDescriptor::CPPTYPE_MESSAGE:
    {
        Message *child = rep ? refl->AddMessage(msg, fd) : refl->MutableMessage(msg, fd);
        randomize(child, r, depth - 1);
        break;
    }
    }
}

void
randomize(Message *msg, rng &r, int depth)
{
    if (depth <= 0)
        return;

    const auto *desc = msg->GetDescriptor();
    const Reflection *refl = msg->GetReflection();

    for (int i = 0; i < desc->field_count(); i++)
    {
        const FieldDescriptor *fd = desc->field(i);

        if (fd->is_map())
        {
            // Unique keys by construction: entry key = sequence number.
            uint64_t n = r.below(4);
            for (uint64_t k = 0; k < n; k++)
            {
                Message *entry = refl->AddMessage(msg, fd);
                const auto *ed = entry->GetDescriptor();
                const Reflection *er = entry->GetReflection();
                const FieldDescriptor *kf = ed->field(0);

                if (kf->cpp_type() == FieldDescriptor::CPPTYPE_STRING)
                    er->SetString(entry, kf, std::to_string(k) + random_ascii(r));
                else if (kf->cpp_type() == FieldDescriptor::CPPTYPE_INT32)
                    er->SetInt32(entry, kf, static_cast<int32_t>(k));
                else
                    er->SetInt64(entry, kf, static_cast<int64_t>(k));

                random_field_value(entry, er, ed->field(1), r, depth);
            }

            continue;
        }

        if (fd->is_repeated())
        {
            uint64_t n = r.below(4);
            for (uint64_t k = 0; k < n; k++)
                random_field_value(msg, refl, fd, r, depth);

            continue;
        }

        /* Singular: present with probability 1/2. */
        if ((r.next() & 1) != 0)
            random_field_value(msg, refl, fd, r, depth);
    }
}

std::span<const std::byte>
as_bytes(const std::string &wire)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte *>(wire.data()), wire.size());
}

template <typename Msg, typename ParseInto>
bool
random_round_trips(const char *label, int iterations, uint64_t seed, ParseInto parse_into)
{
    rng r(seed);

    for (int i = 0; i < iterations; i++)
    {
        Msg original;
        randomize(&original, r, /*depth=*/5);

        const std::string wire = original.SerializePartialAsString();

        Msg decoded;
        const ppb_error err = parse_into(as_bytes(wire), &decoded);
        if (err != PPB_OK)
        {
            fprintf(stderr, "%s[%d]: parse_into failed (ppb_error %d, %zu wire bytes)\n", label, i,
                static_cast<int>(err), wire.size());
            return false;
        }

        if (!MessageDifferencer::Equals(original, decoded))
        {
            fprintf(stderr, "%s[%d]: decoded differs from original (%zu wire bytes)\n", label, i,
                wire.size());
            fprintf(stderr, "original:\n%s\ndecoded:\n%s\n", original.DebugString().c_str(),
                decoded.DebugString().c_str());
            return false;
        }
    }

    return true;
}

}  // namespace

#define RANDOM_CHECK(LABEL, TYPE, PARSE_INTO, N, SEED) \
    random_round_trips<TYPE>(LABEL, (N), (SEED),       \
        [](std::span<const std::byte> b, TYPE *m) { return PARSE_INTO(b, m); })

int
main()
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    bool ok = true;
    ok &= RANDOM_CHECK("pbscalars3.Scalars", demo_pb::Scalars, ppb_gen::demo_pb::Scalars::parse_into, 600,
        0x1234567);
    ok &= RANDOM_CHECK("nested2.Outer", demo2::Outer, ppb_gen::demo2::Outer::parse_into, 600, 0x2234567);
    ok &= RANDOM_CHECK("pbcomposite3.Maps", demo_comp3::Maps, ppb_gen::demo_comp3::Maps::parse_into, 600,
        0x3234567);
    ok &= RANDOM_CHECK("pbcomposite3.Repeated", demo_comp3::Repeated,
        ppb_gen::demo_comp3::Repeated::parse_into, 600, 0x4234567);
    ok &= RANDOM_CHECK("pbcomposite3.Enums", demo_comp3::Enums, ppb_gen::demo_comp3::Enums::parse_into, 600,
        0x4434567);
    ok &= RANDOM_CHECK("pbcomposite3.Fixeds", demo_comp3::Fixeds, ppb_gen::demo_comp3::Fixeds::parse_into,
        600, 0x4534567);
    ok &= RANDOM_CHECK("pbcomposite2.UEnums", demo_comp2::UEnums, ppb_gen::demo_comp2::UEnums::parse_into,
        600, 0x5234567);
    ok &= RANDOM_CHECK("pbcomposite2.UFixeds", demo_comp2::UFixeds, ppb_gen::demo_comp2::UFixeds::parse_into,
        600, 0x5334567);
    ok &= RANDOM_CHECK("pbrecursive.Node", demo_recp::Node, ppb_gen::demo_recp::Node::parse_into, 400,
        0x6234567);
    ok &= RANDOM_CHECK("pbmrec.A", demo_mrec::A, ppb_gen::demo_mrec::A::parse_into, 400, 0x7234567);
    ok &= RANDOM_CHECK("pbrrec.Tree", demo_rrec::Tree, ppb_gen::demo_rrec::Tree::parse_into, 400, 0x8234567);
    ok &= RANDOM_CHECK("oneof3.HasOneof", pkg::HasOneof, ppb_gen::pkg::HasOneof::parse_into, 600, 0x9234567);
    ok &= RANDOM_CHECK("empty3.HasEmpty", demo_empty::HasEmpty, ppb_gen::demo_empty::HasEmpty::parse_into,
        200, 0xA234567);
    ok &= RANDOM_CHECK("wkt_user3.All", wktdemo::All, ppb_gen::wktdemo::All::parse_into, 400, 0xB234567);

    if (ok)
    {
        printf("libprotobuf_random_diff: all clean\n");
    }

    return ok ? 0 : 1;
}
