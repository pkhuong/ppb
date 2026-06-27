import pytest
from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import make_file_proto

T = d.FieldDescriptorProto


def _file_referencing_wkt():
    # A message with one ordinary field and one field referencing an
    # *unsupported* WKT message (Struct), plus the WKT file dependency.
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="n", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    m.field.add(
        name="st",
        number=2,
        type=T.TYPE_MESSAGE,
        label=T.LABEL_OPTIONAL,
        type_name=".google.protobuf.Struct",
    )
    fp = make_file_proto(m)
    fp.dependency.append("google/protobuf/struct.proto")
    return fp


def test_wkt_field_referenced_without_optin_raises_named_flag():
    with pytest.raises(gen.GenError, match="drop_foreign_type_fields"):
        gen.build_model([_file_referencing_wkt()])


def test_skip_wkt_drops_wkt_field_keeps_others():
    model = gen.build_model([_file_referencing_wkt()], skip_wkt=True)
    foo = model.message(".pkg.Foo")
    names = [f.name for f in foo.fields]
    assert names == ["n"]
    assert any(df.name == "st" for df in foo.dropped_fields)


def _make_struct_fp():
    """Return a minimal *unsupported* WKT-like dependency under google/protobuf/.

    It is skipped; a supported WKT file would instead be walked into the model.
    """
    st = d.DescriptorProto(name="Struct")
    st.field.add(name="fields", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    return make_file_proto(st, name="google/protobuf/struct.proto", package="google.protobuf")


def _make_user_fp():
    """User file that happens to live under google/protobuf/."""
    msg = d.DescriptorProto(name="MyMsg")
    msg.field.add(name="value", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    return make_file_proto(msg, name="google/protobuf/mymsg.proto", package="pkg")


def _make_timestamp_fp():
    ts = d.DescriptorProto(name="Timestamp")
    ts.field.add(name="seconds", number=1, type=T.TYPE_INT64, label=T.LABEL_OPTIONAL)
    ts.field.add(name="nanos", number=2, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    return make_file_proto(ts, name="google/protobuf/timestamp.proto", package="google.protobuf")


def _file_referencing_timestamp():
    m = d.DescriptorProto(name="Event")
    m.field.add(name="n", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    m.field.add(
        name="at",
        number=2,
        type=T.TYPE_MESSAGE,
        label=T.LABEL_OPTIONAL,
        type_name=".google.protobuf.Timestamp",
    )
    fp = make_file_proto(m)
    fp.dependency.append("google/protobuf/timestamp.proto")
    return fp


def test_supported_wkt_field_is_kept_and_resolves():
    model = gen.build_model([_make_timestamp_fp(), _file_referencing_timestamp()])
    event = model.message(".pkg.Event")
    assert [f.name for f in event.fields] == ["n", "at"]
    assert model.symbols.has_message(".google.protobuf.Timestamp")
    assert (
        model.symbols.message_schema(".google.protobuf.Timestamp")
        == "::ppb_gen::google::protobuf::Timestamp::schema"
    )


def test_unsupported_wkt_field_still_rejected_by_name():
    m = d.DescriptorProto(name="Has")
    m.field.add(
        name="v",
        number=1,
        type=T.TYPE_MESSAGE,
        label=T.LABEL_OPTIONAL,
        type_name=".google.protobuf.Struct",
    )
    fp = make_file_proto(m)
    fp.dependency.append("google/protobuf/struct.proto")
    with pytest.raises(gen.GenError, match="drop_foreign_type_fields"):
        gen.build_model([fp])


def test_supported_wkt_import_redirects_to_fused_header():
    ts_fp = _make_timestamp_fp()
    user_fp = _file_referencing_timestamp()
    model = gen.build_model([ts_fp, user_fp])
    plan = gen.plan_emission(model, opaque_recursion=False)
    out = gen.emit_file(
        model,
        user_fp,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=plan,
        files_to_generate=("a.proto",),
    )
    assert out.count("#include <ppb/wkt.ppb.hpp>") == 1
    assert '#include "google/protobuf/timestamp.ppb.hpp"' not in out


def _emit_event_schema(strict_repeated_encoding, detect_unknown):
    model = gen.build_model([_make_timestamp_fp(), _file_referencing_timestamp()])
    plan = gen.plan_emission(model, opaque_recursion=False)
    return gen.emit_file(
        model,
        _file_referencing_timestamp(),
        strict_repeated_encoding=strict_repeated_encoding,
        detect_unknown=detect_unknown,
        plan=plan,
    )


def test_strict_schema_defines_and_uses_wkt_unknowns_alias():
    # Strict mode defines a catch-all-bearing alias for the WKT once (the only
    # place the auto_schema wrapper appears) and the message<> descriptor
    # references it by name.
    out = _emit_event_schema(False, detect_unknown=True)
    assert (
        "    using schema_with_unknowns = "
        "::ppb::auto_schema<schema, ::ppb::detect_unknown_fields<>>;" in out
    )
    assert "namespace ppb_gen::google::protobuf::Timestamp" in out
    assert (
        "::ppb::message<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema_with_unknowns, ::ppb::field_semantics::singular>"
        in out
    )


def test_lean_schema_leaves_wkt_message_descriptor_bare():
    out = _emit_event_schema(True, detect_unknown=False)
    assert (
        "::ppb::message<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema, ::ppb::field_semantics::singular>"
        in out
    )
    assert "auto_schema<::ppb_gen::google::protobuf::Timestamp::merge_schema" not in out


def test_requested_file_under_wkt_prefix_not_skipped():
    """A file under google/protobuf/ that is in files_to_generate must not be skipped."""
    struct_fp = _make_struct_fp()
    my_fp = _make_user_fp()
    model = gen.build_model(
        [struct_fp, my_fp],
        skip_wkt=True,
        files_to_generate={"google/protobuf/mymsg.proto"},
    )
    assert model.message(".pkg.MyMsg") is not None
    assert not model.symbols.has_message(".google.protobuf.Struct")


def test_inject_wkt_present_copy_wins():
    # A descriptor-set copy of a supported WKT is not overwritten by the runtime one.
    from google.protobuf.compiler import plugin_pb2

    sentinel = d.DescriptorProto(name="Timestamp")
    sentinel.field.add(name="marker", number=99, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    ts_fp = make_file_proto(
        sentinel, name="google/protobuf/timestamp.proto", package="google.protobuf"
    )
    req = plugin_pb2.CodeGeneratorRequest()
    req.proto_file.append(ts_fp)
    gen._inject_supported_wkt(req)
    timestamps = [fp for fp in req.proto_file if fp.name == "google/protobuf/timestamp.proto"]
    assert len(timestamps) == 1
    assert any(f.name == "marker" for f in timestamps[0].message_type[0].field)
    names = {fp.name for fp in req.proto_file}
    assert "google/protobuf/duration.proto" in names


def test_mixed_supported_unsupported_drops_only_unsupported():
    m = d.DescriptorProto(name="Mix")
    m.field.add(
        name="at",
        number=1,
        type=T.TYPE_MESSAGE,
        label=T.LABEL_OPTIONAL,
        type_name=".google.protobuf.Timestamp",
    )
    m.field.add(
        name="st",
        number=2,
        type=T.TYPE_MESSAGE,
        label=T.LABEL_OPTIONAL,
        type_name=".google.protobuf.Struct",
    )
    fp = make_file_proto(m)
    fp.dependency.extend(["google/protobuf/timestamp.proto", "google/protobuf/struct.proto"])
    model = gen.build_model([_make_timestamp_fp(), fp], skip_wkt=True)
    mix = model.message(".pkg.Mix")
    assert [f.name for f in mix.fields] == ["at"]
    assert any(df.name == "st" for df in mix.dropped_fields)


def test_wkt_file_in_file_to_generate_gets_per_dep_include():
    # A proto importing timestamp AND generating timestamp.proto itself: the dep
    # is carved out (normal per-dep include, not the fused redirect).
    ts_fp = _make_timestamp_fp()
    user_fp = _file_referencing_timestamp()
    model = gen.build_model(
        [ts_fp, user_fp],
        files_to_generate={"a.proto", "google/protobuf/timestamp.proto"},
    )
    plan = gen.plan_emission(model, opaque_recursion=False)
    out = gen.emit_file(
        model,
        user_fp,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=plan,
        files_to_generate=("a.proto", "google/protobuf/timestamp.proto"),
    )
    assert '#include "google/protobuf/timestamp.ppb.hpp"' in out
    assert "#include <ppb/wkt.ppb.hpp>" not in out
