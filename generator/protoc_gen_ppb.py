#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["protobuf>=4"]
# ///
"""protoc-gen-ppb: emit ppb::schema headers from a .proto descriptor closure."""

import dataclasses
import os
import sys

# Force the pure python (instead of upb) protobuf runtime to enable parsing
# heavily nested schemas: upb craps out on schemas that protoc will happily
# generate.  We still have the interpreter and protobuf recursion limits,
# so it's not like we're accepting unbounded recursion either.
os.environ["PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION"] = "python"

from google.protobuf.internal import decoder as _pb_decoder

# Request decode, build_model, emission_order, and compute_depths all
# recurse over the message graph; protoc imposes no depth limit, so a
# pathologically deep closure could blow Python's default 1000-frame
# ceiling.
sys.setrecursionlimit(max(sys.getrecursionlimit(), 10_000))

# pyrefly: ignore[missing-attribute]  # absent from the stubs, present at runtime
if hasattr(_pb_decoder, "SetRecursionLimit"):
    _pb_decoder.SetRecursionLimit(2_000)

# Every generated message/enum namespace nests under this dedicated toplevel
# namespace.  A proto `package pkg` with message `Msg` becomes
# `ppb_gen::pkg::Msg`.  It is a *sibling* of the `ppb` library namespace, not
# nested inside it: we don't awnt proto packages named `schema`/`reader`/etc.
# to introduce collisions.
_GEN_NS = "ppb_gen"

# C++ keywords (and a few context-sensitive identifiers) that can't be used
# verbatim as identifiers in generated code. Matches protobuf's cpp kKeywords
# approach: a colliding identifier gets a single trailing underscore.
_CPP_KEYWORDS = frozenset(
    {
        "alignas",
        "alignof",
        "and",
        "and_eq",
        "asm",
        "auto",
        "bitand",
        "bitor",
        "bool",
        "break",
        "case",
        "catch",
        "char",
        "char8_t",
        "char16_t",
        "char32_t",
        "class",
        "compl",
        "concept",
        "const",
        "consteval",
        "constexpr",
        "constinit",
        "const_cast",
        "continue",
        "co_await",
        "co_return",
        "co_yield",
        "decltype",
        "default",
        "delete",
        "do",
        "double",
        "dynamic_cast",
        "else",
        "enum",
        "explicit",
        "export",
        "extern",
        "false",
        "float",
        "for",
        "friend",
        "goto",
        "if",
        "inline",
        "int",
        "long",
        "mutable",
        "namespace",
        "new",
        "noexcept",
        "not",
        "not_eq",
        "nullptr",
        "operator",
        "or",
        "or_eq",
        "private",
        "protected",
        "public",
        "register",
        "reinterpret_cast",
        "requires",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "static_assert",
        "static_cast",
        "struct",
        "switch",
        "template",
        "this",
        "thread_local",
        "throw",
        "true",
        "try",
        "typedef",
        "typeid",
        "typename",
        "union",
        "unsigned",
        "using",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
        "while",
        "xor",
        "xor_eq",
        "NULL",
    }
)


def cpp_ident(name):
    """Return a proto identifier as a valid C++ identifier (keyword -> name + '_')."""
    return name + "_" if name in _CPP_KEYWORDS else name


def resolve_identifier(name, *, taken):
    """Append underscores until `name` is free of the `taken` set."""
    while name in taken:
        name += "_"

    return name


@dataclasses.dataclass(frozen=True)
class MessageIdentifiers:
    f: str
    schema: str
    merge_schema: str
    max_depth: str


def resolve_message_identifiers(*, sibling_type_names):
    """Resolve every injected name against sibling types.

    Covers the schema header's F / schema / merge_schema / max_depth.
    A nested type of the same name becomes a C++ namespace in the same scope,
    which is ill-formed ("redeclared as a different kind of entity"), so
    we try to find the identifiers as they were renamed.
    """
    taken = frozenset(sibling_type_names)
    f = resolve_identifier("F", taken=taken)
    schema = resolve_identifier("schema", taken=taken)
    merge_schema = resolve_identifier("merge_schema", taken=taken)
    max_depth = resolve_identifier("max_depth", taken=taken)
    return MessageIdentifiers(
        f=f,
        schema=schema,
        merge_schema=merge_schema,
        max_depth=max_depth,
    )


class GenError(Exception):
    """Raised for inputs protoc-gen-ppb deliberately rejects."""


def _cpp_ns(proto_full_name):
    """Convert a proto-qualified name to a C++ namespace.

    ".pkg.Foo.Bar" -> "ppb_gen::pkg::Foo::Bar", keyword components
    -> "..._". The empty proto name (toplevel, no package) maps to the
    bare _GEN_NS.
    """
    parts = [cpp_ident(p) for p in proto_full_name.strip(".").split(".") if p]
    return "::".join([_GEN_NS, *parts])


def _check_no_mangling_collisions(scope, names):
    """`names` is an iterable of raw proto identifiers in one C++ scope.

    Raise GenError if two distinct names collide after cpp_ident().
    """
    seen = {}
    for name in names:
        ident = cpp_ident(name)
        if ident in seen and seen[ident] != name:
            raise GenError(
                f"{scope}: colliding identifiers: {seen[ident]!r} and {name!r} both map to "
                f"{ident!r} after C++-keyword mangling"
            )
        seen[ident] = name
