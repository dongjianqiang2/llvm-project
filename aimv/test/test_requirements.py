"""T0.1 — Verify that ``pip install -r`` succeeds for driver and
mcp-server requirements files.

These tests run ``pip install -r`` in a subprocess to confirm that every
dependency listed in the requirements files is both syntactically valid
and resolvable from the configured index.

Tests are automatically skipped when:
* ``pip`` is not available on ``sys.executable``.
* The ``CI_NO_NETWORK`` environment variable is set (common in sandboxed
  CI runners that block outbound HTTPS).
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest


def _aimv_root() -> Path:
    """Return the absolute path to the ``aimv/`` top-level directory."""
    return Path(__file__).resolve().parent.parent.parent


_PIP_AVAILABLE: bool | None = None


def _check_pip() -> bool:
    """Return ``True`` if ``python -m pip`` is usable."""
    global _PIP_AVAILABLE
    if _PIP_AVAILABLE is not None:
        return _PIP_AVAILABLE

    try:
        result = subprocess.run(
            [sys.executable, "-m", "pip", "--version"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        _PIP_AVAILABLE = result.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        _PIP_AVAILABLE = False

    return _PIP_AVAILABLE


def _skip_if_no_network() -> None:
    """Skip the current test when CI_NO_NETWORK is set."""
    if os.environ.get("CI_NO_NETWORK"):
        pytest.skip("CI_NO_NETWORK is set — skipping network-dependent test")


def _skip_if_no_pip() -> None:
    """Skip the current test when pip is not available."""
    if not _check_pip():
        pytest.skip("pip is not available on this interpreter")


# ---------------------------------------------------------------------------
# Requirements file paths (relative to aimv root)
# ---------------------------------------------------------------------------
REQUIREMENTS_FILES = [
    "aimv/driver/requirements.txt",
    "aimv/mcp_server/requirements.txt",
]


class TestRequirementsInstall:
    """Run ``pip install -r`` against each requirements file to verify
    that all dependencies are syntactically valid and resolvable."""

    @pytest.mark.parametrize("rel_path", REQUIREMENTS_FILES)
    def test_pip_install_requirements(self, rel_path: str) -> None:
        _skip_if_no_pip()
        _skip_if_no_network()

        full = _aimv_root() / rel_path
        assert full.is_file(), f"Requirements file not found: {full}"

        cmd = [
            sys.executable,
            "-m",
            "pip",
            "install",
            "--dry-run",
            "--ignore-installed",
            "--no-deps",
            "-r",
            str(full),
        ]

        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=120,
        )

        assert result.returncode == 0, (
            f"pip install -r {rel_path} failed "
            f"(rc={result.returncode}):\n\n"
            f"STDERR:\n{result.stderr}\n\n"
            f"STDOUT:\n{result.stdout}"
        )
