"""T3.8 — Source extraction helper tests."""
import sys
import tempfile
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.aimv_driver import (
    extract_function_source, extract_function_signature,
    extract_loop_line,
)


MULTI_FUNC_SRC = """\
void foo(int n) {
    for (int i = 0; i < n; i++) {
        a[i] = b[i];
    }
}

int bar(int x) {
    return x + 1;
}
"""


@pytest.fixture
def c_file():
    with tempfile.NamedTemporaryFile(mode="w", suffix=".c", delete=False) as f:
        f.write(MULTI_FUNC_SRC)
        path = f.name
    yield str(path)
    Path(path).unlink(missing_ok=True)


class TestExtractFunctionSource:
    def test_extract_foo(self, c_file):
        src = extract_function_source(c_file, "foo")
        assert src is not None
        assert "void foo" in src
        assert "a[i] = b[i]" in src

    def test_extract_bar(self, c_file):
        src = extract_function_source(c_file, "bar")
        assert src is not None
        assert "int bar" in src
        assert "return x + 1" in src

    def test_nonexistent_function(self, c_file):
        src = extract_function_source(c_file, "nonexistent")
        assert src is None


class TestExtractFunctionSignature:
    def test_extract_foo_signature(self, c_file):
        sig = extract_function_signature(c_file, "foo")
        assert "void foo(int n)" in sig

    def test_unknown_function(self, c_file):
        sig = extract_function_signature(c_file, "unknown")
        assert sig == "unknown"  # fallback


class TestExtractLoopLine:
    def test_valid_location(self):
        diags = [{"function_name": "foo", "loop_location": "test.c:42:5"}]
        assert extract_loop_line(diags, "foo") == 42

    def test_empty_diagnostics(self):
        # [AIMV] Fallback is 1 (file start) since FunctionInfo.loop_line requires gt=0
        assert extract_loop_line([], "foo") == 1

    def test_no_match(self):
        diags = [{"function_name": "bar", "loop_location": "test.c:1:1"}]
        assert extract_loop_line(diags, "foo") == 1


class TestExtractLinesAround:
    """extract_lines_around was removed in T3.8 rewrite. Skip these tests."""
    pass
