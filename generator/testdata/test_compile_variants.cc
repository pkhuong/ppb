/*
 * Includes one representative variant golden from each of several distinct protos
 * (all have unique C++ namespaces) and forces schema instantiation so the
 * static_asserts inside ppb::auto_schema<> actually run.  Compile-time test only.
 *
 * Variant golden          top-level namespace(s) covered
 * ---------------------------------------------------------------------
 * foreigndrop2.drop_imports ppb_gen::demo2::WithStruct (drop variant)
 * leanrep3.none           ppb_gen::demo_lean        (.none, error fallback for packed)
 * pbcomposite3.full       ppb_gen::demo_comp3       (.full, detect_unknown_fields<> inline)
 * ppbshadow3              ppb_gen::ppb::M           (shadow: proto package named "ppb")
 * ppbwkt3                 ppb_gen::google::protobuf::Timestamp
 *                         ppb_gen::ppb::Event       (WKT _with_unknowns aliases)
 * scalars3.detect_unknown ppb_gen::demo             (detect_unknown variant)
 * stdshadow_rec3          ppb_gen::std::Node        (shadow: proto package named "std")
 */
#include "golden/foreigndrop2.drop_imports.ppb.hpp"
#include "golden/leanrep3.none.ppb.hpp"
#include "golden/pbcomposite3.full.ppb.hpp"
#include "golden/ppbshadow3.ppb.hpp"
#include "golden/ppbwkt3.ppb.hpp"
#include "golden/scalars3.detect_unknown.ppb.hpp"
#include "golden/stdshadow_rec3.ppb.hpp"

/* pbcomposite3.full: .full variant with detect_unknown_fields<> baked into every schema */
static_assert(sizeof(ppb_gen::demo_comp3::Enums::schema) > 0);
static_assert(sizeof(ppb_gen::demo_comp3::Enums::merge_schema) > 0);
static_assert(sizeof(ppb_gen::demo_comp3::Item::schema) > 0);
static_assert(sizeof(ppb_gen::demo_comp3::Item::merge_schema) > 0);
static_assert(sizeof(ppb_gen::demo_comp3::Repeated::schema) > 0);
static_assert(sizeof(ppb_gen::demo_comp3::Repeated::merge_schema) > 0);

/* leanrep3.none: .none variant; packed repeated uses error fallback descriptor */
static_assert(sizeof(ppb_gen::demo_lean::Rep::schema) > 0);
static_assert(sizeof(ppb_gen::demo_lean::Rep::merge_schema) > 0);

/*
 * scalars3.detect_unknown: detect_unknown variant; schema + merge_schema has
 * detect_unknown_fields<> and a field_semantics::error fallback for packed_int32
 */
static_assert(sizeof(ppb_gen::demo::Scalars::schema) > 0);
static_assert(sizeof(ppb_gen::demo::Scalars::merge_schema) > 0);

/* foreigndrop2.drop_imports: drop variant; dropped WKT field leaves only F::id */
static_assert(sizeof(ppb_gen::demo2::WithStruct::schema) > 0);
static_assert(sizeof(ppb_gen::demo2::WithStruct::merge_schema) > 0);

/*
 * ppbshadow3: proto package literally named "ppb"; schema must compile despite
 * the name collision with the ppb:: library namespace
 */
static_assert(sizeof(ppb_gen::ppb::M::schema) > 0);
static_assert(sizeof(ppb_gen::ppb::M::merge_schema) > 0);

/*
 * stdshadow_rec3: proto package literally named "std"; schema must compile
 * despite the name collision with the ::std namespace
 */
static_assert(sizeof(ppb_gen::std::Node::schema) > 0);
static_assert(sizeof(ppb_gen::std::Node::merge_schema) > 0);

/*
 * ppbwkt3: WKT-referencing message; exercises _with_unknowns aliases added by
 * the generator on top of the shipped wkt.ppb.hpp base schemas
 */
static_assert(sizeof(ppb_gen::google::protobuf::Timestamp::schema_with_unknowns) > 0);
static_assert(sizeof(ppb_gen::google::protobuf::Timestamp::merge_schema_with_unknowns) > 0);
static_assert(sizeof(ppb_gen::ppb::Event::schema) > 0);
static_assert(sizeof(ppb_gen::ppb::Event::merge_schema) > 0);
