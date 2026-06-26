import protoc_gen_ppb as gen


def test_cpp_ident_appends_underscore_for_keywords():
    assert gen.cpp_ident("int") == "int_"
    assert gen.cpp_ident("class") == "class_"
    assert gen.cpp_ident("counts") == "counts"


def test_keyword_message_name_mangled_in_namespace():
    assert gen._cpp_ns(".pkg.class.Inner") == "ppb_gen::pkg::class_::Inner"
