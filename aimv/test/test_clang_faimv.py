# [AIMV] T1.7 — clang Driver -faimv flag integration tests
import json
import os
import subprocess
import tempfile

import pytest

CLANG_BIN = os.environ.get(
    "AIMV_CLANG_BIN",
    os.path.join(os.path.dirname(__file__), "..", "..", "build", "bin", "clang"),
)
CLANG_BIN = os.path.abspath(CLANG_BIN)

ARM_TARGET = "armv7-unknown-linux-gnueabi"

ALIAS_TEST_C = """\
void process_task(int *a, int *b, int n) {
  for (int i = 0; i < n; i++)
    a[i] = b[i] + b[i + 1];
}
"""

# Variable trip count so the vectorizer runs and produces !aimv.diag
VECTOR_TEST_C = """\
void foo(int *a, int *b, int n) {
  for (int i = 0; i < n; i++)
    a[i] = b[i];
}
"""


def _clang_available():
    return os.path.isfile(CLANG_BIN) and os.access(CLANG_BIN, os.X_OK)


skip_no_clang = pytest.mark.skipif(
    not _clang_available(),
    reason=f"clang not found at {CLANG_BIN}",
)


def _run_clang(args, input_text=None):
    """Run clang, return stdout+stderr combined. -### returns non-zero, so don't check."""
    result = subprocess.run(
        [CLANG_BIN] + args,
        input=input_text,
        capture_output=True,
        text=True,
    )
    return result.stdout + result.stderr


def _compile_c(source_code, extra_flags, tmpdir, obj_name="test.o"):
    """Compile C source and return (obj_path, aimv_json_path)."""
    src = os.path.join(tmpdir, "test.c")
    obj = os.path.join(tmpdir, obj_name)
    with open(src, "w") as f:
        f.write(source_code)
    subprocess.check_call(
        [CLANG_BIN, "-O2", f"--target={ARM_TARGET}"] + extra_flags + ["-c", src, "-o", obj],
        stderr=subprocess.DEVNULL,
    )
    return obj, obj + ".aimv.json"


@skip_no_clang
class TestFaimvFlagRegistration:
    """T1.7: -faimv / -fno-aimv registered in Options.td."""

    def test_faimv_in_help(self):
        out = subprocess.check_output(
            [CLANG_BIN, "--help"], stderr=subprocess.STDOUT, text=True
        )
        assert "-faimv" in out
        assert "-fno-aimv" in out

    def test_faimv_help_text(self):
        out = subprocess.check_output(
            [CLANG_BIN, "--help"], stderr=subprocess.STDOUT, text=True
        )
        for line in out.splitlines():
            if "-faimv" in line and "Enable" in line:
                assert "AIMV" in line or "vectorization" in line.lower()
                break


@skip_no_clang
class TestFaimvFlagForwarding:
    """T1.7: -faimv forwards -mllvm -aimv-enable + -mllvm -aimv-output."""

    def test_faimv_forwards_mllvm_flags(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            src = os.path.join(tmpdir, "test.c")
            with open(src, "w") as f:
                f.write(VECTOR_TEST_C)
            out = _run_clang(["-O2", "-faimv", f"--target={ARM_TARGET}",
                              "-###", "-c", src, "-o", os.path.join(tmpdir, "test.o")])
            assert "-aimv-enable" in out
            assert "-aimv-output=" in out

    def test_fno_aimv_no_forwarding(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            src = os.path.join(tmpdir, "test.c")
            with open(src, "w") as f:
                f.write(VECTOR_TEST_C)
            out = _run_clang(["-O2", "-fno-aimv", f"--target={ARM_TARGET}",
                              "-###", "-c", src])
            assert "-aimv-enable" not in out
            assert "-aimv-output" not in out

    def test_no_faimv_no_forwarding(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            src = os.path.join(tmpdir, "test.c")
            with open(src, "w") as f:
                f.write(VECTOR_TEST_C)
            out = _run_clang(["-O2", f"--target={ARM_TARGET}", "-###", "-c", src])
            assert "-aimv-enable" not in out
            assert "-aimv-output" not in out

    def test_aimv_output_path_uses_output_filename(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            src = os.path.join(tmpdir, "test.c")
            obj = os.path.join(tmpdir, "myoutput.o")
            with open(src, "w") as f:
                f.write(VECTOR_TEST_C)
            out = _run_clang(["-O2", "-faimv", f"--target={ARM_TARGET}",
                              "-###", "-c", src, "-o", obj])
            assert "myoutput.o.aimv.json" in out


@skip_no_clang
class TestFaimvCompilation:
    """T1.7: clang -O2 -faimv -c produces aimv.json."""

    def test_faimv_produces_aimv_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            obj, aimv_json = _compile_c(VECTOR_TEST_C, ["-faimv"], tmpdir)
            assert os.path.isfile(aimv_json), f"Expected {aimv_json} to exist"
            with open(aimv_json) as f:
                data = json.load(f)
            assert "diagnostics" in data
            assert isinstance(data["diagnostics"], list)

    def test_fno_aimv_no_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            obj, aimv_json = _compile_c(VECTOR_TEST_C, ["-fno-aimv"], tmpdir)
            assert not os.path.isfile(aimv_json)

    def test_no_faimv_no_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            obj, aimv_json = _compile_c(VECTOR_TEST_C, [], tmpdir)
            assert not os.path.isfile(aimv_json)

    def test_faimv_aimv_json_has_target(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            obj, aimv_json = _compile_c(VECTOR_TEST_C, ["-faimv"], tmpdir)
            with open(aimv_json) as f:
                data = json.load(f)
            assert "target" in data
            assert data["target"]["triple"] == ARM_TARGET

    def test_faimv_alias_benchmark(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            obj, aimv_json = _compile_c(ALIAS_TEST_C, ["-faimv"], tmpdir)
            assert os.path.isfile(aimv_json)
            with open(aimv_json) as f:
                data = json.load(f)
            assert "diagnostics" in data
            assert len(data["diagnostics"]) >= 1


@skip_no_clang
class TestFaimvAntiFork:
    """T1.7: -faimv only at Driver layer, -mllvm flags don't include -faimv."""

    def test_mllvm_flags_no_faimv(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            src = os.path.join(tmpdir, "test.c")
            with open(src, "w") as f:
                f.write(VECTOR_TEST_C)
            out = _run_clang(["-O2", "-faimv", f"--target={ARM_TARGET}",
                              "-###", "-c", src, "-o", os.path.join(tmpdir, "test.o")])
            cc1_section = out[out.find('"-cc1"'):] if '"-cc1"' in out else out
            assert '"-faimv"' not in cc1_section

    def test_faimv_and_fno_faimv_last_wins(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            src = os.path.join(tmpdir, "test.c")
            with open(src, "w") as f:
                f.write(VECTOR_TEST_C)
            out = _run_clang(["-O2", "-faimv", "-fno-aimv", f"--target={ARM_TARGET}",
                              "-###", "-c", src])
            assert "-aimv-enable" not in out
