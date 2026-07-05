import pytest
from protoc_gen_ppb import parse_options, GenError


def test_default_mode_is_lean():
    o = parse_options("")
    assert o.strict_repeated_encoding is True
    assert not o.detect_unknown


def test_mode_values():
    assert parse_options("mode=none").strict_repeated_encoding is False
    assert parse_options("mode=none").detect_unknown is False
    assert parse_options("mode=full").strict_repeated_encoding is False
    assert parse_options("mode=full").detect_unknown is True


def test_full_expands_to_detect_unknown():
    assert parse_options("mode=full").detect_unknown is True


def test_renamed_flags():
    o = parse_options(
        "opaque_cycles,oneof_as_optional,"
        "drop_foreign_type_fields,drop_group_extension_fields,detect_unknown"
    )
    assert o.opaque_cycles and o.oneof_as_optional
    assert o.drop_foreign_type_fields and o.drop_group_extension_fields
    assert o.detect_unknown


def test_always_dispatch_strings_flag():
    assert parse_options("always_dispatch_strings").always_dispatch_strings is True
    assert parse_options("").always_dispatch_strings is False


def test_unknown_mode_rejected():
    with pytest.raises(GenError):
        parse_options("mode=bogus")


def test_strict_repeated_encoding_flag():
    # Most useful as an override when a mode= preset relaxes it
    # (e.g. mode=none,strict_repeated_encoding).
    assert parse_options("strict_repeated_encoding").strict_repeated_encoding is True
    # From default (lean), the flag is idempotent.
    o = parse_options("")
    assert o.strict_repeated_encoding is True
    # Overrides mode=none's relaxed encoding back to strict.
    o = parse_options("mode=none,strict_repeated_encoding")
    assert o.strict_repeated_encoding is True
    # Overrides mode=full's relaxed encoding back to strict while keeping detection.
    o = parse_options("mode=full,strict_repeated_encoding")
    assert o.strict_repeated_encoding is True
    assert o.detect_unknown is True


def test_old_flag_names_rejected():
    for old in (
        "lean",
        "strict",
        "skip_unsupported_fields",
        "opaque_recursion",
    ):
        with pytest.raises(GenError):
            parse_options(old)
