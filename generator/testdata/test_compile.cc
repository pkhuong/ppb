/*
 * Includes generated golden headers and forces schema instantiation so
 * ppb::schema's static_asserts run. Compiling cleanly == passing.
 */
#include "golden/empty3.ppb.hpp"
#include "golden/enumref3.ppb.hpp"
#include "golden/keywords3.ppb.hpp"
#include "golden/leanrep3.ppb.hpp"
#include "golden/maps3.ppb.hpp"
#include "golden/nested2.ppb.hpp"
#include "golden/nsclash3.ppb.hpp"
#include "golden/pbscalars3.ppb.hpp"
#include "golden/recursive3.ppb.hpp"
#include "golden/scalars3.ppb.hpp"
#include "golden/stdshadow3.ppb.hpp"
#include "golden/xfile_base3.ppb.hpp"
#include "golden/xfile_main3.ppb.hpp"

/*
 * Force implicit instantiation (a bare using-alias does not evaluate
 * the static_asserts inside the schema template).
 *
 * All generated namespaces nest under ppb_gen:: so they never collide with
 * libprotobuf's generated `class pkg::Msg` in a shared translation unit.
 */
static_assert(sizeof(ppb_gen::demo_empty::Empty::schema) > 0);
static_assert(sizeof(ppb_gen::demo_empty::HasEmpty::schema) > 0);
static_assert(ppb_gen::demo_empty::HasEmpty::max_depth == 1);
static_assert(sizeof(ppb_gen::demo_kw::Class::schema) > 0);
static_assert(sizeof(ppb_gen::demo::Scalars::schema) > 0);
static_assert(sizeof(ppb_gen::demo_pb::Scalars::schema) > 0);
static_assert(sizeof(ppb_gen::demo2::Outer::schema) > 0);
static_assert(sizeof(ppb_gen::demo2::Outer::Inner::schema) > 0);
static_assert(sizeof(ppb_gen::demo_rec::Node::schema) > 0);
static_assert(sizeof(ppb_gen::a::b::a::schema) > 0);
static_assert(sizeof(ppb_gen::a::b::Outer::schema) > 0);
// A namespace segment literally named `std`: the emitted `::std::int32_t` /
// `::std::size_t` must resolve to the global std, not to `ppb_gen::demo::std`.
static_assert(sizeof(ppb_gen::demo::std::Foo::schema) > 0);
// lean-mode golden: repeated fields keep their canonical packed descriptor plus
// a field_semantics::error fallback; instantiating the schema compiles both.
static_assert(sizeof(ppb_gen::demo_lean::Rep::schema) > 0);
static_assert(sizeof(ppb_gen::demo_map::WithMap::schema) > 0);
static_assert(sizeof(ppb_gen::demo_map::WithMap::CountsEntry::schema) > 0);
// A message referencing an enum nested in a later-declared message: the schema
// only compiles because emission order places the enum's owner first.
static_assert(sizeof(ppb_gen::demo_enumref::Uses::schema) > 0);
static_assert(sizeof(ppb_gen::demo_enumref::Defs::schema) > 0);
// Cross-file closure: Main references another file's message schema, file-scope
// enum, and a message's nested enum.
static_assert(sizeof(ppb_gen::xfile_base::Leaf::schema) > 0);
static_assert(sizeof(ppb_gen::xfile_main::Main::schema) > 0);
static_assert(ppb_gen::xfile_main::Main::max_depth == 1);  // submessage depth spans the file

// Spot-check the emitted max_depth constants are usable in constant context.
static_assert(ppb_gen::demo2::Outer::max_depth == 1);
static_assert(ppb_gen::demo_rec::Node::max_depth == 0);  // self-edge is opaque
static_assert(ppb_gen::demo_map::WithMap::max_depth == 1);  // map entry is one submessage level

int
main()
{
    return 0;
}
