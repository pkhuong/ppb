import pytest
from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import make_file_proto as fp

T = d.FieldDescriptorProto


def msg_with_msg_fields(name, refs):
    """refs: list of (field_name, number, target_proto_full_name)."""
    m = d.DescriptorProto(name=name)
    for fname, num, target in refs:
        m.field.add(
            name=fname, number=num, type=T.TYPE_MESSAGE, label=T.LABEL_OPTIONAL, type_name=target
        )
    return m


def test_topo_order_dependencies_before_dependents():
    top = msg_with_msg_fields("Top", [("m", 1, ".pkg.Mid")])
    mid = msg_with_msg_fields("Mid", [("l", 1, ".pkg.Leaf")])
    leaf = d.DescriptorProto(name="Leaf")
    model = gen.build_model([fp(top, mid, leaf)])
    order = gen.emission_order(model, opaque_recursion=False)
    names = [m.full_name for m in order]
    assert names.index(".pkg.Leaf") < names.index(".pkg.Mid") < names.index(".pkg.Top")


def test_topo_ignores_enum_references():
    # Enum-typed fields never constrain emission order (nested enum defs are
    # hoisted ahead of every schema), so a message naming a later message's
    # nested enum still emits in plain declaration order.
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
    model = gen.build_model([fp(uses, defs)])
    order = gen.emission_order(model, opaque_recursion=False)
    assert [m.full_name for m in order] == [".pkg.Uses", ".pkg.Defs"]


def test_topo_preserves_idl_order_for_independent_messages():
    a = d.DescriptorProto(name="A")
    b = d.DescriptorProto(name="B")
    c = d.DescriptorProto(name="C")
    model = gen.build_model([fp(a, b, c)])
    order = gen.emission_order(model, opaque_recursion=False)
    assert [m.full_name for m in order] == [".pkg.A", ".pkg.B", ".pkg.C"]


def test_max_depth():
    top = msg_with_msg_fields("Top", [("m", 1, ".pkg.Mid")])
    mid = msg_with_msg_fields("Mid", [("l", 1, ".pkg.Leaf")])
    leaf = d.DescriptorProto(name="Leaf")
    model = gen.build_model([fp(top, mid, leaf)])
    depths = gen.compute_depths(model, opaque_fields=frozenset())
    assert depths[".pkg.Leaf"] == 0
    assert depths[".pkg.Mid"] == 1
    assert depths[".pkg.Top"] == 2


def test_cycle_rejected_without_opaque_recursion():
    a = msg_with_msg_fields("A", [("b", 1, ".pkg.B")])
    b = msg_with_msg_fields("B", [("a", 1, ".pkg.A")])
    model = gen.build_model([fp(a, b)])
    with pytest.raises(gen.GenError, match="cycle"):
        gen.emission_order(model, opaque_recursion=False)


def test_back_edges_identifies_only_true_cycle_edge():
    # A -> B -> A : DFS in decl order from A makes B->A the back-edge.
    a = msg_with_msg_fields("A", [("b", 1, ".pkg.B")])
    b = msg_with_msg_fields("B", [("a", 1, ".pkg.A")])
    model = gen.build_model([fp(a, b)])
    backs = gen.find_back_edges(model)
    assert backs == frozenset({(".pkg.B", "a")})


def test_self_cycle_back_edge():
    node = msg_with_msg_fields("Node", [("next", 1, ".pkg.Node")])
    model = gen.build_model([fp(node)])
    assert gen.find_back_edges(model) == frozenset({(".pkg.Node", "next")})
    depths = gen.compute_depths(model, opaque_fields=frozenset({(".pkg.Node", "next")}))
    assert depths[".pkg.Node"] == 0


def test_three_cycle_cuts_deepest_back_edge():
    # A -> B -> C -> A: DFS in declaration order from A closes the cycle at the
    # deepest edge, C -> A.
    a = msg_with_msg_fields("A", [("b", 1, ".pkg.B")])
    b = msg_with_msg_fields("B", [("c", 1, ".pkg.C")])
    c = msg_with_msg_fields("C", [("a", 1, ".pkg.A")])
    model = gen.build_model([fp(a, b, c)])
    assert gen.find_back_edges(model) == frozenset({(".pkg.C", "a")})
    # Cutting C -> A leaves edges A -> B -> C, so the order is C, B, A.
    order = gen.emission_order(model, opaque_recursion=True)
    names = [m.full_name for m in order]
    assert names.index(".pkg.C") < names.index(".pkg.B") < names.index(".pkg.A")
