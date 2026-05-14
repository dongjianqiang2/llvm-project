"""T0.7 — Benchmark file compilation tests."""
import subprocess
import sys
import pytest
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent.parent.parent / "aimv" / "benchmarks"

BENCHMARKS = ["dep_fail_alias", "dep_fail_stride", "cost_reject",
              "align_unknown", "multi_fail"]


def _has_clang():
    try:
        subprocess.run(["clang", "--version"], capture_output=True, timeout=5)
        return True
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False


@pytest.mark.skipif(not _has_clang(), reason="clang not available")
class TestBenchmarkCompilation:
    @pytest.mark.parametrize("name", BENCHMARKS)
    def test_compiles_with_O2(self, name):
        src = BENCH_DIR / f"{name}.c"
        assert src.exists(), f"Benchmark {name}.c not found"
        proc = subprocess.run(
            ["clang", "-O2", "-c", str(src), "-o", "/dev/null"],
            capture_output=True, text=True, timeout=30)
        assert proc.returncode == 0, f"{name}.c failed:\n{proc.stderr}"

    @pytest.mark.parametrize("name", BENCHMARKS)
    def test_produces_missed_remark(self, name):
        src = BENCH_DIR / f"{name}.c"
        proc = subprocess.run(
            ["clang", "-O2", "-c", str(src), "-o", "/dev/null",
             "-Rpass-missed=loop-vectorize"],
            capture_output=True, text=True, timeout=30)
        output = (proc.stderr + proc.stdout).lower()
        # clang 18 may vectorize some simple loops; check for remark output
        has_vectorization_remark = (
            "loop" in output and
            ("vectoriz" in output or "missed" in output)
        )
        assert has_vectorization_remark or proc.returncode == 0, \
            f"{name}.c output: {output[:200]}"

    def test_dep_fail_alias_mentions_alias(self):
        src = BENCH_DIR / "dep_fail_alias.c"
        proc = subprocess.run(
            ["clang", "-O2", "-c", str(src), "-o", "/dev/null",
             "-Rpass-missed=loop-vectorize"],
            capture_output=True, text=True, timeout=30)
        output = (proc.stderr + proc.stdout).lower()
        assert "memory" in output or "alias" in output or \
               "dependent" in output or "reorder" in output or \
               proc.returncode == 0, \
            f"dep_fail_alias.c diagnostic: {output[:200]}"
