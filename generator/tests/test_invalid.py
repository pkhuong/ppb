import shutil
import subprocess
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path

import pytest

from google.protobuf import descriptor_pb2 as d
from google.protobuf.compiler import plugin_pb2 as p
import protoc_gen_ppb as gen

HERE = Path(__file__).resolve().parent
INVALID = HERE.parent / "testdata" / "invalid"

pytestmark = pytest.mark.skipif(shutil.which("protoc") is None, reason="protoc not installed")


def _request_for(proto_name, tmp_path):
    """Compile <proto_name> to a descriptor set and wrap it in a CodeGeneratorRequest.

    This lets the test exercise generate() directly.
    """
    out = tmp_path / "fds.pb"
    subprocess.run(
        [
            "protoc",
            f"--proto_path={INVALID}",
            f"--descriptor_set_out={out}",
            "--include_imports",
            proto_name,
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    fds = d.FileDescriptorSet.FromString(out.read_bytes())
    req = p.CodeGeneratorRequest()
    req.proto_file.extend(fds.file)
    req.file_to_generate.append(proto_name)
    opt = (INVALID / proto_name).with_suffix(".opt")
    if opt.exists():
        req.parameter = opt.read_text().strip()

    return req


def _cases(suffix):
    return sorted(f.stem for f in INVALID.glob(f"*.{suffix}"))


@pytest.mark.parametrize("name", _cases("expected-error"))
def test_error_cases(name, tmp_path):
    req = _request_for(f"{name}.proto", tmp_path)
    buf = StringIO()
    with redirect_stderr(buf):
        resp = gen.generate(req)

    expected = (INVALID / f"{name}.expected-error").read_text()
    assert resp.error == expected.rstrip("\n")
    assert len(resp.file) == 0


@pytest.mark.parametrize("name", _cases("expected-stderr"))
def test_warning_cases(name, tmp_path):
    req = _request_for(f"{name}.proto", tmp_path)
    buf = StringIO()
    with redirect_stderr(buf):
        resp = gen.generate(req)

    expected = (INVALID / f"{name}.expected-stderr").read_text()
    assert buf.getvalue() == expected
    assert resp.error == ""
    assert len(resp.file) >= 1
