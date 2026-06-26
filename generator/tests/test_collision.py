import pytest
from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import make_file_proto as file_with

T = d.FieldDescriptorProto


def test_field_name_collision_after_mangling_raises():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="class", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    foo.field.add(name="class_", number=2, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    with pytest.raises(gen.GenError, match=r"class.*class_.*class_|collid"):
        gen.build_model([file_with(foo)])


def test_enum_value_collision_after_mangling_raises():
    e = d.EnumDescriptorProto(name="E")
    e.value.add(name="new", number=0)
    e.value.add(name="new_", number=1)
    foo = d.DescriptorProto(name="Foo", enum_type=[e])
    with pytest.raises(gen.GenError, match="collid"):
        gen.build_model([file_with(foo)])


def test_sibling_type_collision_after_mangling_raises():
    inner1 = d.DescriptorProto(name="delete")
    inner2 = d.DescriptorProto(name="delete_")
    foo = d.DescriptorProto(name="Foo", nested_type=[inner1, inner2])
    with pytest.raises(gen.GenError, match="collid"):
        gen.build_model([file_with(foo)])


def test_no_collision_when_names_distinct_after_mangling():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="class", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    foo.field.add(name="ok", number=2, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    gen.build_model([file_with(foo)])  # 'class'->'class_', 'ok'->'ok'; no clash


def test_cross_file_same_package_collision_after_mangling_raises():
    # Two files in one package: `new` mangles to `new_`, colliding with the
    # other file's literal `new_`; both would emit namespace pkg::new_.
    fa = file_with(d.DescriptorProto(name="new"), name="a.proto")
    fb = file_with(d.DescriptorProto(name="new_"), name="b.proto")
    with pytest.raises(gen.GenError, match="collid"):
        gen.build_model([fa, fb])


def test_cross_file_same_package_enum_collision_raises():
    ea = d.EnumDescriptorProto(name="try")
    eb = d.EnumDescriptorProto(name="try_")
    fa = file_with(d.DescriptorProto(name="A"), name="a.proto")
    fa.enum_type.append(ea)
    fb = file_with(d.DescriptorProto(name="B"), name="b.proto")
    fb.enum_type.append(eb)
    with pytest.raises(gen.GenError, match="collid"):
        gen.build_model([fa, fb])


def test_cross_file_distinct_package_no_collision():
    fa = file_with(d.DescriptorProto(name="new"), name="a.proto", package="p1")
    fb = file_with(d.DescriptorProto(name="new_"), name="b.proto", package="p2")
    gen.build_model([fa, fb])  # distinct namespaces; must not raise
