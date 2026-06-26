import pytest
from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import make_file_proto as file_with_message

T = d.FieldDescriptorProto


def test_accept_proto3_synthetic_optional_oneof():
    m = d.DescriptorProto(name="Foo")
    m.oneof_decl.add(name="_a")
    fld = m.field.add(name="a", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, oneof_index=0)
    fld.proto3_optional = True
    model = gen.build_model([file_with_message(m)])  # must not raise
    assert model.message(".pkg.Foo")


def test_accept_proto3_optional_field_at_nonzero_position():
    # Regression: a proto3 `optional` field whose declaration position differs
    # from its synthetic oneof index must still be accepted. (Earlier the
    # oneof check compared field positions against oneof indices and only
    # passed when they coincided at 0.)
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="plain", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    m.oneof_decl.add(name="_opt")
    fld = m.field.add(
        name="opt", number=2, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, oneof_index=0
    )
    fld.proto3_optional = True
    model = gen.build_model([file_with_message(m)])  # must not raise
    assert model.message(".pkg.Foo")


def test_reject_editions_via_edition_field():
    # Older editions descriptors leave syntax empty and only set `edition`.
    fp = d.FileDescriptorProto(name="a.proto", package="pkg")
    fp.edition = d.Edition.EDITION_2023
    fp.message_type.add(name="Foo").field.add(
        name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL
    )
    with pytest.raises(gen.GenError, match="Editions"):
        gen.build_model([fp])


def test_reject_editions_via_syntax_string():
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="editions")
    fp.message_type.add(name="Foo").field.add(
        name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL
    )
    with pytest.raises(gen.GenError, match="Editions"):
        gen.build_model([fp])


def test_accept_proto2_without_edition():
    # A normal proto2 file (empty syntax, no edition) still builds.
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    model = gen.build_model([file_with_message(m, syntax="")])  # must not raise
    assert model.message(".pkg.Foo")


def test_reject_field_number_zero():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="x", number=0, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    with pytest.raises(gen.GenError, match="out of range"):
        gen.build_model([file_with_message(m)])


def test_reject_field_number_above_max():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="x", number=1 << 29, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    with pytest.raises(gen.GenError, match="out of range"):
        gen.build_model([file_with_message(m)])


def test_accept_field_number_at_max():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="x", number=(1 << 29) - 1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    model = gen.build_model([file_with_message(m)])  # must not raise
    assert model.message(".pkg.Foo")


def test_reject_duplicate_field_number():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="a", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    m.field.add(name="b", number=1, type=T.TYPE_STRING, label=T.LABEL_OPTIONAL)
    with pytest.raises(gen.GenError, match="duplicate field number"):
        gen.build_model([file_with_message(m)])


def test_reject_field_number_reserved_low():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="x", number=19000, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    with pytest.raises(gen.GenError, match="reserved"):
        gen.build_model([file_with_message(m)])


def test_reject_field_number_reserved_high():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="x", number=19999, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    with pytest.raises(gen.GenError, match="reserved"):
        gen.build_model([file_with_message(m)])


def test_accept_field_number_just_below_reserved():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="a", number=18999, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    m.field.add(name="b", number=20000, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    model = gen.build_model([file_with_message(m)])  # must not raise
    assert model.message(".pkg.Foo")


def test_proto3_optional_without_oneof_index_does_not_mask_real_oneof():
    # A crafted descriptor flagging proto3_optional but omitting oneof_index
    # must not contaminate the synthetic set with a spurious 0 and thereby mask
    # a real oneof declared at index 0.  The real oneof must still be rejected.
    m = d.DescriptorProto(name="Foo")
    m.oneof_decl.add(name="choice")
    m.field.add(name="a", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, oneof_index=0)
    m.field.add(name="b", number=2, type=T.TYPE_STRING, label=T.LABEL_OPTIONAL, oneof_index=0)
    bad = m.field.add(name="c", number=3, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    bad.proto3_optional = True  # no oneof_index set
    with pytest.raises(gen.GenError, match="oneof is unsupported"):
        gen.build_model([file_with_message(m)])


def test_reject_oneof_index_out_of_range():
    # With oneofs allowed, an oneof_index beyond oneof_decl must raise a clear
    # GenError rather than an opaque IndexError caught by the broad handler.
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="a", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, oneof_index=2)
    with pytest.raises(gen.GenError, match="oneof index"):
        gen.build_model([file_with_message(m)], allow_oneof=True)
