"""T4.5 — Performance measurement tests."""
import sys
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.perf_measurer import (
    measure_with_clock, compute_speedup, _has_perf,
)


class TestPerfMeasurement:
    def test_measure_returns_positive(self):
        """Measure a trivial command returns positive elapsed time."""
        ms = measure_with_clock("/bin/true", warmup=1, runs=3)
        assert ms > 0

    def test_compute_speedup_positive(self):
        """10% faster = positive speedup."""
        speedup = compute_speedup(100.0, 90.0)
        assert speedup == pytest.approx(10.0)

    def test_compute_speedup_negative(self):
        """10% slower = negative speedup."""
        speedup = compute_speedup(100.0, 110.0)
        assert speedup == pytest.approx(-10.0)

    def test_compute_speedup_zero_optimized(self):
        """Zero optimized time = 0% speedup."""
        speedup = compute_speedup(100.0, 0.0)
        assert speedup == 0.0

    def test_has_perf_returns_bool(self):
        """_has_perf returns a boolean."""
        result = _has_perf()
        assert isinstance(result, bool)


class TestSpeedupThreshold:
    """Verify the 5% threshold from SPEC §8.1 is testable."""
    def test_above_threshold(self):
        speedup = compute_speedup(100.0, 94.0)
        assert speedup >= 5.0

    def test_below_threshold(self):
        speedup = compute_speedup(100.0, 96.0)
        assert speedup < 5.0

    def test_exactly_threshold(self):
        speedup = compute_speedup(100.0, 95.0)
        assert speedup == pytest.approx(5.0)
