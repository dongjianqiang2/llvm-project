# [AIMV] AIMV Driver — Performance measurement (T4.5)
"""Performance measurement using perf stat or clock_gettime."""
import subprocess
import statistics
import time
import os
from pathlib import Path
from typing import Optional


def measure_with_perf(binary: str, warmup: int = 3, runs: int = 10) -> Optional[float]:
    """Measure execution time using Linux perf stat.

    Returns median elapsed time in milliseconds, or None if perf is unavailable.
    """
    if not _has_perf():
        return None

    # Warmup
    for _ in range(warmup):
        subprocess.run(["perf", "stat", binary],
                       capture_output=True, timeout=30)

    elapsed_times = []
    for _ in range(runs):
        start = time.monotonic()
        proc = subprocess.run(["perf", "stat", binary],
                              capture_output=True, text=True, timeout=30)
        elapsed = (time.monotonic() - start) * 1000
        # Parse task-clock from perf output
        task_clock = _parse_perf_task_clock(proc.stderr)
        elapsed_times.append(task_clock if task_clock else elapsed)

    return statistics.median(elapsed_times)


def measure_with_clock(binary: str, warmup: int = 3, runs: int = 10) -> float:
    """Measure execution time using clock_gettime wrapper.

    Falls back to subprocess timing if the binary doesn't self-report.
    """
    for _ in range(warmup):
        subprocess.run([binary], capture_output=True, timeout=30)

    times = []
    for _ in range(runs):
        start = time.monotonic()
        subprocess.run([binary], capture_output=True, timeout=30)
        times.append((time.monotonic() - start) * 1000)

    return statistics.median(times)


def measure(binary: str, method: str = "auto",
            warmup: int = 3, runs: int = 10) -> float:
    """Measure execution time, auto-selecting best available method."""
    if method == "perf" or (method == "auto" and _has_perf()):
        result = measure_with_perf(binary, warmup, runs)
        if result is not None:
            return result
    return measure_with_clock(binary, warmup, runs)


def compute_speedup(baseline_ms: float, optimized_ms: float) -> float:
    """Compute speedup ratio (positive = faster)."""
    if optimized_ms <= 0:
        return 0.0
    return ((baseline_ms - optimized_ms) / baseline_ms) * 100.0


def _has_perf() -> bool:
    return shutil_which("perf") is not None


def _parse_perf_task_clock(stderr: str) -> Optional[float]:
    """Parse 'task-clock' from perf stat stderr output."""
    import re
    m = re.search(r'([\d,.]+)\s+msec\s+task-clock', stderr)
    if m:
        return float(m.group(1).replace(',', ''))
    return None


def shutil_which(cmd: str) -> Optional[str]:
    """Find executable in PATH."""
    import shutil as _shutil
    return _shutil.which(cmd)
