import pytest
from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import make_file_proto as file_with

T = d.FieldDescriptorProto


def test_unresolved_message_type_raises_generror():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(
        name="m",
        number=1,
        type=T.TYPE_MESSAGE,
        label=T.LABEL_OPTIONAL,
        type_name=".pkg.DoesNotExist",
    )
    with pytest.raises(gen.GenError, match="undefined message type .pkg.DoesNotExist"):
        gen.build_model([file_with(foo)])


def test_unresolved_enum_type_raises_generror():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(
        name="e", number=1, type=T.TYPE_ENUM, label=T.LABEL_OPTIONAL, type_name=".pkg.NoSuchEnum"
    )
    with pytest.raises(gen.GenError, match="undefined enum type .pkg.NoSuchEnum"):
        gen.build_model([file_with(foo)])


def test_resolved_types_do_not_raise():
    bar = d.DescriptorProto(name="Bar")
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(
        name="b", number=1, type=T.TYPE_MESSAGE, label=T.LABEL_OPTIONAL, type_name=".pkg.Bar"
    )
    fp = file_with(foo)
    fp.message_type.append(bar)
    gen.build_model([fp])  # must not raise
