import protoc_gen_ppb as gen


def test_module_exposes_version():
    assert isinstance(gen.PLUGIN_NAME, str)
    assert gen.PLUGIN_NAME == "protoc-gen-ppb"
