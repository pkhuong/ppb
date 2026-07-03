/*
 * Round-trip differential for the reflection sink (protoc-gen-ppb-reflect).
 *
 * For each input message: build a libprotobuf message, serialize it, decode
 * the same bytes with the generated `parse_into` into a fresh message, and
 * assert the two are field-by-field equal.
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
#include <cstdio>
#include <google/protobuf/util/message_differencer.h>
#include <span>
#include <string>

namespace
{

using ::google::protobuf::util::MessageDifferencer;

std::span<const std::byte>
as_bytes(const std::string &wire)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte *>(wire.data()), wire.size());
}

/*
 * Serialize `original`, decode the bytes with `parse_into` into a fresh `Message`,
 * and compare.  Returns true on field-by-field equality.
 */
template <typename Message, typename ParseInto>
bool
round_trips(const char *label, const Message &original, ParseInto parse_into)
{
    const std::string wire = original.SerializeAsString();

    Message decoded;
    const ppb_error err = parse_into(as_bytes(wire), &decoded);
    if (err != PPB_OK)
    {
        fprintf(stderr, "%s: parse_into failed (ppb_error %d)\n", label, static_cast<int>(err));
        return false;
    }

    if (!MessageDifferencer::Equals(original, decoded))
    {
        fprintf(stderr, "%s: decoded message differs from the original\n", label);
        return false;
    }

    return true;
}

bool
check_empty()
{
    demo_empty::Empty a;

    return round_trips("empty3.Empty", a, [](std::span<const std::byte> b, demo_empty::Empty *m)
        { return ppb_gen::demo_empty::Empty::parse_into(b, m); });
}

bool
check_scalars()
{
    demo_pb::Scalars a;
    a.set_i(-7);
    a.set_l(-99999999999LL);
    a.set_u(3u);
    a.set_t(40000000000ULL);
    a.set_z(-5);
    a.set_f(0xDEADBEEFu);
    a.set_g(-4567890123LL);
    a.set_fl(1.5f);
    a.set_d(2.5);
    a.set_b(true);
    a.set_s("hello");
    a.set_by(std::string("\x01\x00\x02", 3));  // embedded NUL on purpose
    a.set_maybe(11);
    a.add_packed_vals(10);
    a.add_packed_vals(20);
    a.add_names("x");
    a.add_names("yy");

    return round_trips("pbscalars3", a, [](std::span<const std::byte> b, demo_pb::Scalars *m)
        { return ppb_gen::demo_pb::Scalars::parse_into(b, m); });
}

bool
check_nested()
{
    demo2::Outer a;
    a.mutable_inner()->set_a(42);
    a.add_unpacked_vals(7);
    a.add_unpacked_vals(8);
    a.add_packed_vals(9);
    a.add_packed_vals(10);

    return round_trips("nested2", a, [](std::span<const std::byte> b, demo2::Outer *m)
        { return ppb_gen::demo2::Outer::parse_into(b, m); });
}

bool
check_enums()
{
    demo_comp3::Enums a;
    a.set_c(demo_comp3::GREEN);
    a.add_cs(demo_comp3::RED);
    a.add_cs(demo_comp3::GREEN);

    return round_trips("pbcomposite3.Enums", a, [](std::span<const std::byte> b, demo_comp3::Enums *m)
        { return ppb_gen::demo_comp3::Enums::parse_into(b, m); });
}

bool
check_fixeds()
{
    demo_comp3::Fixeds a;
    a.add_f32s(1u);
    a.add_f32s(0xFFFFFFFFu);
    a.add_s64s(-4567890123LL);
    a.add_fls(1.5f);
    a.add_ds(2.5);

    return round_trips("pbcomposite3.Fixeds", a, [](std::span<const std::byte> b, demo_comp3::Fixeds *m)
        { return ppb_gen::demo_comp3::Fixeds::parse_into(b, m); });
}

bool
check_repeated()
{
    demo_comp3::Repeated a;
    demo_comp3::Item *i0 = a.add_items();
    i0->set_id(1);
    i0->set_label("one");
    demo_comp3::Item *i1 = a.add_items();
    i1->set_id(2);
    i1->set_label("two");

    return round_trips("pbcomposite3.Repeated", a, [](std::span<const std::byte> b, demo_comp3::Repeated *m)
        { return ppb_gen::demo_comp3::Repeated::parse_into(b, m); });
}

bool
check_maps()
{
    demo_comp3::Maps a;
    (*a.mutable_counts())["x"] = 10;
    (*a.mutable_counts())["y"] = 20;
    (*a.mutable_names())[7] = "seven";

    return round_trips("pbcomposite3.Maps", a, [](std::span<const std::byte> b, demo_comp3::Maps *m)
        { return ppb_gen::demo_comp3::Maps::parse_into(b, m); });
}

bool
check_unpacked_enums_fixeds()
{
    demo_comp2::UEnums e;
    e.add_cs(demo_comp2::RED);
    e.add_cs(demo_comp2::UNSET);
    const bool ok_e = round_trips("pbcomposite2.UEnums", e,
        [](std::span<const std::byte> b, demo_comp2::UEnums *m)
        { return ppb_gen::demo_comp2::UEnums::parse_into(b, m); });

    demo_comp2::UFixeds f;
    f.add_f32s(3u);
    f.add_ds(4.5);
    const bool ok_f = round_trips("pbcomposite2.UFixeds", f,
        [](std::span<const std::byte> b, demo_comp2::UFixeds *m)
        { return ppb_gen::demo_comp2::UFixeds::parse_into(b, m); });

    return ok_e & ok_f;
}

bool
check_recursive()
{
    demo_recp::Node a;
    a.set_value(1);
    demo_recp::Node *n = a.mutable_next();
    n->set_value(2);
    n->mutable_next()->set_value(3);  // three levels deep

    return round_trips("pbrecursive.Node", a, [](std::span<const std::byte> b, demo_recp::Node *m)
        { return ppb_gen::demo_recp::Node::parse_into(b, m); });
}

bool
check_mutual_recursive()
{
    demo_mrec::A a;
    a.set_av(1);
    demo_mrec::B *b = a.mutable_b();
    b->set_bv(2);
    b->mutable_a()->set_av(3);  // A -> B -> A, three levels

    return round_trips("pbmrec.A", a, [](std::span<const std::byte> b2, demo_mrec::A *m)
        { return ppb_gen::demo_mrec::A::parse_into(b2, m); });
}

bool
check_repeated_recursive()
{
    demo_rrec::Tree a;
    a.set_value(1);
    demo_rrec::Tree *c0 = a.add_children();
    c0->set_value(2);
    demo_rrec::Tree *c1 = a.add_children();
    c1->set_value(3);
    c1->add_children()->set_value(4);  // nested repeated recursion

    return round_trips("pbrrec.Tree", a, [](std::span<const std::byte> b, demo_rrec::Tree *m)
        { return ppb_gen::demo_rrec::Tree::parse_into(b, m); });
}

/* Parse `expected`'s wire bytes into `*decoded` (subcase 2 pre-seeds it with
 * subcase 1's decoded result, still holding kSub, to prove the oneof case
 * flips) and require field equality plus the expected oneof case. */
bool
oneof_round_trips(const char *label, const pkg::HasOneof &expected, pkg::HasOneof *decoded,
    pkg::HasOneof::ChoiceCase expected_case)
{
    const std::string wire = expected.SerializeAsString();

    const ppb_error err = ppb_gen::pkg::HasOneof::parse_into(as_bytes(wire), decoded);
    if (err != PPB_OK)
    {
        fprintf(stderr, "%s: parse_into failed (ppb_error %d)\n", label, static_cast<int>(err));
        return false;
    }

    if (!MessageDifferencer::Equals(expected, *decoded))
    {
        fprintf(stderr, "%s: decoded message differs from the original\n", label);
        return false;
    }

    if (decoded->choice_case() != expected_case)
    {
        fprintf(stderr, "%s: expected choice_case() == %d\n", label, static_cast<int>(expected_case));
        return false;
    }

    return true;
}

bool
check_oneof()
{
    pkg::HasOneof expected_sub;
    expected_sub.mutable_sub()->set_v(7);
    pkg::HasOneof decoded_sub;
    const bool ok_sub = oneof_round_trips("oneof3.HasOneof/sub", expected_sub, &decoded_sub,
        pkg::HasOneof::kSub);

    pkg::HasOneof expected_num;
    expected_num.set_num(5);
    const bool ok_num = ok_sub &&
        oneof_round_trips("oneof3.HasOneof/num", expected_num, &decoded_sub, pkg::HasOneof::kNum);

    pkg::HasOneof expected_col;
    expected_col.set_col(pkg::GREEN);
    pkg::HasOneof decoded_col;
    const bool ok_col = ok_num &&
        oneof_round_trips("oneof3.HasOneof/col", expected_col, &decoded_col, pkg::HasOneof::kCol);

    pkg::HasOneof expected_text;
    expected_text.set_text("hi");
    pkg::HasOneof decoded_text;

    return ok_col &&
        oneof_round_trips("oneof3.HasOneof/text", expected_text, &decoded_text, pkg::HasOneof::kText);
}

bool
check_wkt()
{
    /* Fully populate every supported WKT field of wktdemo.All. */
    wktdemo::All a;
    a.mutable_at()->set_seconds(1718000000);
    a.mutable_at()->set_nanos(123456789);
    a.mutable_took()->set_seconds(42);
    a.mutable_took()->set_nanos(-500);
    a.mutable_flag()->set_value(true);
    a.mutable_count()->set_value(-7);
    a.mutable_label()->set_value("hello wkt");
    a.mutable_mask()->add_paths("user.name");
    a.mutable_mask()->add_paths("user.email");

    /*
     * Use a type_url that is deliberately not a well-formed Any URL (no '/'
     * separator) so MessageDifferencer's Any-aware comparison bails out of
     * ParseAnyTypeUrl and byte-compares `value` instead of trying to unpack
     * it.
     */
    a.mutable_blob()->set_type_url("wktdemo.Unresolvable");
    a.mutable_blob()->set_value(std::string("\x01\x00\x02", 3));  // embedded NUL
    a.mutable_nothing();  // mark the field-less Empty present

    return round_trips("wktdemo.All", a, [](std::span<const std::byte> b, wktdemo::All *m)
        { return ppb_gen::wktdemo::All::parse_into(b, m); });
}

}  // namespace

int
main()
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    bool ok = true;

    for (bool (*const check)() : { check_empty, check_scalars, check_nested, check_enums, check_fixeds,
             check_repeated, check_maps, check_unpacked_enums_fixeds, check_recursive, check_mutual_recursive,
             check_repeated_recursive, check_oneof, check_wkt })
    {
        ok &= check();
    }

    return ok ? 0 : 1;
}
