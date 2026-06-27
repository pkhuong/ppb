import shutil
import subprocess
from pathlib import Path
import pytest

HERE = Path(__file__).resolve().parent
GEN = HERE.parent
TESTDATA = GEN / "testdata"


def protoc_available():
    return shutil.which("protoc") is not None


pytestmark = pytest.mark.skipif(not protoc_available(), reason="protoc not installed")


def run_protoc(proto, out_dir, opt=""):
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        "protoc",
        f"--plugin=protoc-gen-ppb={GEN / 'protoc_gen_ppb.py'}",
        f"--proto_path={TESTDATA}",
        f"--ppb_out={out_dir}",
    ]
    if opt:
        cmd.append(f"--ppb_opt={opt}")

    cmd.append(proto)
    return subprocess.run(cmd, capture_output=True, text=True)


def test_scalars_generates_and_matches_golden(tmp_path):
    res = run_protoc("scalars3.proto", tmp_path)
    assert res.returncode == 0, res.stderr
    got = (tmp_path / "scalars3.ppb.hpp").read_text()
    golden = (TESTDATA / "golden" / "scalars3.ppb.hpp").read_text()
    assert got == golden


def test_maps_generates_and_matches_golden(tmp_path):
    res = run_protoc("maps3.proto", tmp_path)
    assert res.returncode == 0, res.stderr
    got = (tmp_path / "maps3.ppb.hpp").read_text()
    golden = (TESTDATA / "golden" / "maps3.ppb.hpp").read_text()
    assert got == golden


def test_enumref_orders_enum_owner_first(tmp_path):
    res = run_protoc("enumref3.proto", tmp_path)
    assert res.returncode == 0, res.stderr
    got = (tmp_path / "enumref3.ppb.hpp").read_text()
    golden = (TESTDATA / "golden" / "enumref3.ppb.hpp").read_text()
    assert got == golden
    # The owning message's namespace must precede the referrer's, otherwise the
    # ::demo_enumref::Defs::Color reference would not compile.
    assert got.index("namespace ppb_gen::demo_enumref::Defs") < got.index(
        "namespace ppb_gen::demo_enumref::Uses"
    )


def test_cross_file_closure_generates_and_matches_golden(tmp_path):
    # Both files in one invocation: drives generate()'s multi-file loop and the
    # global emission order spanning the file boundary.
    tmp_path.mkdir(parents=True, exist_ok=True)
    res = subprocess.run(
        [
            "protoc",
            f"--plugin=protoc-gen-ppb={GEN / 'protoc_gen_ppb.py'}",
            f"--proto_path={TESTDATA}",
            f"--ppb_out={tmp_path}",
            "xfile_base3.proto",
            "xfile_main3.proto",
        ],
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, res.stderr
    for name in ("xfile_base3.ppb.hpp", "xfile_main3.ppb.hpp"):
        got = (tmp_path / name).read_text()
        golden = (TESTDATA / "golden" / name).read_text()
        assert got == golden, name
    main = (tmp_path / "xfile_main3.ppb.hpp").read_text()
    assert '#include "xfile_base3.ppb.hpp"' in main
    # Merge is unconditional: a singular cross-file submessage references the
    # owner's merge_schema with singular semantics so split occurrences merge.
    assert (
        "ppb::message<F::leaf, ::ppb_gen::xfile_base::Leaf::merge_schema, "
        "::ppb::field_semantics::singular>" in main
    )
    assert "ppb::proto3_enumerated<F::kind, ::ppb_gen::xfile_base::Leaf::Kind>" in main


def test_recursive_rejected_by_default(tmp_path):
    res = run_protoc("recursive3.proto", tmp_path)
    assert res.returncode != 0
    assert "cycle" in res.stderr


def test_recursive_opaque_succeeds(tmp_path):
    res = run_protoc("recursive3.proto", tmp_path, opt="opaque_cycles")
    assert res.returncode == 0, res.stderr
    text = (tmp_path / "recursive3.ppb.hpp").read_text()
    assert "recursive: opaque" in text


def test_none_and_full_share_wire_policy(tmp_path):
    # Both mode=none and mode=full accept alternate wire forms: packed canonical
    # (the field's proto3 encoding) plus a bare-int32 fallback with semantics
    # always_lexn.  Their wire policies are identical (full only adds detection).
    run_protoc("scalars3.proto", tmp_path / "none", opt="mode=none")
    run_protoc("scalars3.proto", tmp_path / "full", opt="mode=full")
    none = (tmp_path / "none" / "scalars3.ppb.hpp").read_text()
    full = (tmp_path / "full" / "scalars3.ppb.hpp").read_text()
    fallback = "ppb::int32<F::packed_vals, ::ppb::field_semantics::always_lexn>"
    for text in (none, full):
        assert "packed_int32<F::packed_vals>" in text
        assert fallback in text


def test_lean_default_rejects_unexpected_wire_form(tmp_path):
    # The default mode is lean: keep the canonical packed descriptor but make the
    # unpacked fallback fail closed via field_semantics::error.
    run_protoc("scalars3.proto", tmp_path / "lean")
    lean = (tmp_path / "lean" / "scalars3.ppb.hpp").read_text()
    assert "packed_int32<F::packed_vals>" in lean
    assert "ppb::int32<F::packed_vals, ::ppb::field_semantics::error>" in lean
