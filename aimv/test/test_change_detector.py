"""T6.1 — Change detector tests."""
import sys
import tempfile
import subprocess
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.ci.change_detector import (
    _get_function_definitions_regex,
    _get_changed_line_ranges,
    _ranges_overlap,
    filter_loopy_functions,
)


SRC_MULTI_FUNC = """\
void foo(int n) {
    for (int i = 0; i < n; i++) a[i] = b[i];
}

int bar(int x) {
    return x + 1;
}

static void baz(void) {
    int x = 0;
}
"""


@pytest.fixture
def c_file():
    with tempfile.NamedTemporaryFile(mode="w", suffix=".c", delete=False) as f:
        f.write(SRC_MULTI_FUNC)
        path = f.name
    yield str(path)
    Path(path).unlink(missing_ok=True)


class TestRegexFunctionDetection:
    def test_detects_foo(self, c_file):
        funcs = _get_function_definitions_regex(c_file)
        names = [f[0] for f in funcs]
        assert "foo" in names

    def test_detects_bar(self, c_file):
        funcs = _get_function_definitions_regex(c_file)
        names = [f[0] for f in funcs]
        assert "bar" in names

    def test_detects_baz(self, c_file):
        funcs = _get_function_definitions_regex(c_file)
        names = [f[0] for f in funcs]
        assert "baz" in names

    def test_no_keywords_as_names(self, c_file):
        funcs = _get_function_definitions_regex(c_file)
        names = [f[0] for f in funcs]
        assert "if" not in names
        assert "for" not in names
        assert "while" not in names

    def test_line_range_positive(self, c_file):
        funcs = _get_function_definitions_regex(c_file)
        for name, start, end in funcs:
            assert start > 0
            assert end >= start


class TestChangedLineRanges:
    def test_empty_for_no_changes(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".c", delete=False) as f:
            f.write("// no changes\n")
            path = f.name
        try:
            ranges = _get_changed_line_ranges(path, "HEAD", "HEAD")
            assert isinstance(ranges, list)
        finally:
            Path(path).unlink(missing_ok=True)


class TestRangesOverlap:
    def test_overlap(self):
        assert _ranges_overlap(10, 20, 15, 25) is True

    def test_no_overlap(self):
        assert _ranges_overlap(10, 20, 21, 30) is False

    def test_adjacent(self):
        assert _ranges_overlap(10, 20, 20, 30) is True

    def test_contained(self):
        assert _ranges_overlap(10, 50, 20, 30) is True


class TestFilterLoopyFunctions:
    def test_returns_list(self, c_file):
        import shutil
        if not shutil.which("opt"):
            pytest.skip("opt not available in PATH")
        funcs = [("test_func", "foo", 1, 3)]
        result = filter_loopy_functions(funcs)
        assert isinstance(result, list)
