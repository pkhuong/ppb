import pytest
from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import make_file_proto

T = d.FieldDescriptorProto


def test_emit_file_scope_enum_is_defined_and_referenced():
    color = d.EnumDescriptorProto(name="Color")
    color.value.add(name="RED", number=0)
    color.value.add(name="GREEN", number=1)
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(
        name="c", number=1, type=T.TYPE_ENUM, label=T.LABEL_OPTIONAL, type_name=".pkg.Color"
    )
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.enum_type.append(color)
    fp.message_type.append(foo)
    model = gen.build_model([fp])

    text = gen.emit_file(
        model,
        fp,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=gen.plan_emission(model, opaque_recursion=False),
    )
    # The top-level enum must be defined before the message that references it.
    assert "namespace ppb_gen::pkg\n{\n    enum class Color : ::std::int32_t" in text
    assert "RED = 0," in text
    assert text.index("enum class Color") < text.index("namespace ppb_gen::pkg::Foo")
    assert "ppb::proto3_enumerated<F::c, ::ppb_gen::pkg::Color>" in text


def test_nested_enum_defined_before_referrer_schema():
    # `Uses` (declared first) names an enum nested in `Defs` (declared later).
    # The nested enum is hoisted ahead of every schema alias, so it is defined
    # before `Uses`'s schema names it even though `Uses`'s schema block is
    # emitted first.
    uses = d.DescriptorProto(name="Uses")
    uses.field.add(
        name="color",
        number=1,
        type=T.TYPE_ENUM,
        label=T.LABEL_OPTIONAL,
        type_name=".pkg.Defs.Color",
    )
    defs = d.DescriptorProto(name="Defs")
    defs.enum_type.add(name="Color").value.add(name="UNSET", number=0)
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.extend([uses, defs])
    model = gen.build_model([fp])

    text = gen.emit_file(
        model,
        fp,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=gen.plan_emission(model, opaque_recursion=False),
    )
    assert "namespace ppb_gen::pkg::Defs\n{\n    enum class Color : ::std::int32_t" in text
    assert text.index("enum class Color") < text.index("ppb::proto3_enumerated<F::color")
    # The referrer's schema block still precedes the owner's (declaration order;
    # the enum reference adds no dependency edge).
    assert text.index("namespace ppb_gen::pkg::Uses") < text.rindex("namespace ppb_gen::pkg::Defs")


def test_mutual_enum_reference_is_representable():
    # Two messages each naming an enum nested in the other. Hoisting both enum
    # definitions ahead of both schema aliases makes this representable -- there
    # is no cycle, because enums depend on nothing.
    a = d.DescriptorProto(name="A")
    a.field.add(name="x", number=1, type=T.TYPE_ENUM, label=T.LABEL_OPTIONAL, type_name=".pkg.B.E")
    a.enum_type.add(name="E").value.add(name="Z", number=0)
    b = d.DescriptorProto(name="B")
    b.field.add(name="y", number=1, type=T.TYPE_ENUM, label=T.LABEL_OPTIONAL, type_name=".pkg.A.E")
    b.enum_type.add(name="E").value.add(name="Z", number=0)
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.extend([a, b])
    model = gen.build_model([fp])

    text = gen.emit_file(
        model,
        fp,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=gen.plan_emission(model, opaque_recursion=False),
    )
    # Both enum definitions precede both schema aliases.
    assert text.rindex("enum class E") < text.index("using schema")
    assert "ppb::proto3_enumerated<F::x, ::ppb_gen::pkg::B::E>" in text
    assert "ppb::proto3_enumerated<F::y, ::ppb_gen::pkg::A::E>" in text


def test_emit_file_has_clang_format_off():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="n", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    fp = make_file_proto(foo)
    model = gen.build_model([fp])
    plan = gen.plan_emission(model, opaque_recursion=False)

    out = gen.emit_file(model, fp, strict_repeated_encoding=True, detect_unknown=False, plan=plan)
    assert "// clang-format off" in out
    assert out.index("// clang-format off") < out.index("namespace ")


def test_emit_file_header_and_includes():
    leaf = d.DescriptorProto(name="Leaf")
    top = d.DescriptorProto(name="Top")
    top.field.add(
        name="l", number=1, type=T.TYPE_MESSAGE, label=T.LABEL_OPTIONAL, type_name=".pkg.Leaf"
    )
    fp = d.FileDescriptorProto(
        name="sub/a.proto", package="pkg", syntax="proto3", dependency=["dep/b.proto"]
    )
    fp.message_type.extend([top, leaf])
    model = gen.build_model([fp])

    text = gen.emit_file(
        model,
        fp,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=gen.plan_emission(model, opaque_recursion=False),
    )

    assert text.startswith("#pragma once\n")
    assert "#include <ppb/ppb.hpp>" in text
    assert '#include "dep/b.ppb.hpp"' in text
    # Leaf must be emitted before Top (dependency order).
    assert text.index("namespace ppb_gen::pkg::Leaf") < text.index("namespace ppb_gen::pkg::Top")
    assert "::ppb::message<F::l, ::ppb_gen::pkg::Leaf::merge_schema" in text


def test_emit_file_only_emits_its_own_messages():
    # Two files; emitting file a must not contain file b's message body.
    a_msg = d.DescriptorProto(name="A")
    fa = d.FileDescriptorProto(name="a.proto", package="pa", syntax="proto3")
    fa.message_type.append(a_msg)
    b_msg = d.DescriptorProto(name="B")
    fb = d.FileDescriptorProto(name="b.proto", package="pb", syntax="proto3")
    fb.message_type.append(b_msg)
    model = gen.build_model([fa, fb])
    text = gen.emit_file(
        model,
        fa,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=gen.plan_emission(model, opaque_recursion=False),
    )
    assert "namespace ppb_gen::pa::A" in text
    assert "namespace ppb_gen::pb::B" not in text


def test_cross_file_reference_uses_mangled_schema_name():
    # File b defines Other, which nests a type named `schema`, forcing Other's
    # schema alias to mangle to `schema_`. File a references Other across the
    # file boundary and must use the same mangled name.
    schema_nested = d.DescriptorProto(name="schema")
    other = d.DescriptorProto(name="Other", nested_type=[schema_nested])
    fb = d.FileDescriptorProto(name="b.proto", package="pb", syntax="proto3")
    fb.message_type.append(other)

    user = d.DescriptorProto(name="User")
    user.field.add(
        name="o", number=1, type=T.TYPE_MESSAGE, label=T.LABEL_OPTIONAL, type_name=".pb.Other"
    )
    fa = d.FileDescriptorProto(
        name="a.proto", package="pa", syntax="proto3", dependency=["b.proto"]
    )
    fa.message_type.append(user)

    model = gen.build_model([fa, fb])
    text = gen.emit_file(
        model,
        fa,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=gen.plan_emission(model, opaque_recursion=False),
    )
    assert '#include "b.ppb.hpp"' in text
    assert (
        "ppb::message<F::o, ::ppb_gen::pb::Other::merge_schema, ::ppb::field_semantics::singular>"
        in text
    )


def test_emit_file_skips_wkt_include_when_flagged():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="n", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    m.field.add(
        name="dur",
        number=2,
        type=T.TYPE_MESSAGE,
        label=T.LABEL_OPTIONAL,
        type_name=".google.protobuf.Duration",
    )
    fp = make_file_proto(m)
    fp.dependency.append("google/protobuf/duration.proto")

    model = gen.build_model([fp], skip_wkt=True)
    text = gen.emit_file(
        model,
        fp,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=gen.plan_emission(model, opaque_recursion=False),
        skip_wkt=True,
    )
    assert "#include <ppb/ppb.hpp>" in text
    assert "google/protobuf/duration" not in text


def test_emit_file_emits_dropped_field_comment():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="n", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    m.field.add(
        name="dur",
        number=2,
        type=T.TYPE_MESSAGE,
        label=T.LABEL_OPTIONAL,
        type_name=".google.protobuf.Duration",
    )
    fp = make_file_proto(m)
    fp.dependency.append("google/protobuf/duration.proto")

    model = gen.build_model([fp], skip_wkt=True)
    text = gen.emit_file(
        model,
        fp,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=gen.plan_emission(model, opaque_recursion=False),
        skip_wkt=True,
    )
    assert (
        "//   ppb-dropped: .pkg.Foo.dur (references well-known type .google.protobuf.Duration)"
        in text
    )


def test_emit_file_raises_on_wkt_dep_without_flag():
    # Message has only ordinary fields, so build_model succeeds without skip_wkt.
    # The file-level *unsupported* WKT dependency is still present and must cause
    # emit_file to raise when skip_wkt=False.
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="n", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    fp = make_file_proto(m)
    fp.dependency.append("google/protobuf/struct.proto")

    model = gen.build_model([fp])
    plan = gen.plan_emission(model, opaque_recursion=False)

    with pytest.raises(gen.GenError, match="drop_foreign_type_fields"):
        gen.emit_file(
            model,
            fp,
            strict_repeated_encoding=True,
            detect_unknown=False,
            plan=plan,
            skip_wkt=False,
        )


def test_emit_file_logs_dropped_group_and_ignored_extension_range():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="n", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    m.field.add(name="g", number=2, type=T.TYPE_GROUP, label=T.LABEL_OPTIONAL, type_name=".pkg.G")
    er = m.extension_range.add()
    er.start = 120
    er.end = 201
    fp = make_file_proto(m)
    model = gen.build_model([fp], skip_unsupported_fields=True)
    plan = gen.plan_emission(model, opaque_recursion=False)
    out = gen.emit_file(model, fp, strict_repeated_encoding=False, detect_unknown=True, plan=plan)
    assert "ppb-dropped: .pkg.Foo.g (group field" in out
    assert "ppb-extension-range-ignored: 120-200" in out


def test_emit_file_merge_is_unconditional_no_lww_divergence_note():
    # Merge of split singular submessages is always on now, so the old
    # last-write-wins divergence comment must never appear.
    bar = d.DescriptorProto(name="Bar")
    bar.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(
        name="b", number=1, type=T.TYPE_MESSAGE, label=T.LABEL_OPTIONAL, type_name=".pkg.Bar"
    )
    fp = make_file_proto(bar, foo)
    model = gen.build_model([fp])
    plan = gen.plan_emission(model, opaque_recursion=False)

    text = gen.emit_file(model, fp, strict_repeated_encoding=True, detect_unknown=False, plan=plan)
    assert "last-write-wins" not in text
    # Merge schema is emitted so a singular submessage merges field-by-field.
    assert "merge_schema" in text


def test_emit_file_logs_ignored_extension_def():
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto2")
    other = fp.message_type.add(name="Other")
    other.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    foo = fp.message_type.add(name="Foo")
    foo.extension.add(
        name="e", number=1000, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, extendee=".pkg.Other"
    )
    model = gen.build_model([fp], skip_unsupported_fields=True)
    plan = gen.plan_emission(model, opaque_recursion=False)
    out = gen.emit_file(model, fp, strict_repeated_encoding=True, detect_unknown=False, plan=plan)
    assert "ppb-extension-def-ignored: .pkg.Foo.e" in out
