import protoc_gen_ppb as gen


def test_no_collision_keeps_name():
    assert gen.resolve_identifier("F", taken=frozenset({"Color", "Bar"})) == "F"


def test_single_collision_adds_underscore():
    assert gen.resolve_identifier("F", taken=frozenset({"F"})) == "F_"


def test_repeated_collision_adds_more_underscores():
    assert gen.resolve_identifier("schema", taken=frozenset({"schema", "schema_"})) == "schema__"


def test_resolve_all_three_against_sibling_types():
    names = gen.resolve_message_identifiers(sibling_type_names=frozenset({"F", "max_depth"}))
    assert names.f == "F_"
    assert names.schema == "schema"
    assert names.max_depth == "max_depth_"
