"""T0.1 — Verify the AIMV directory structure and Python package skeleton.

These tests confirm that every required directory and ``__init__.py`` file
exists under the ``aimv/`` package root.
"""

import pytest
from pathlib import Path


def _aimv_root() -> Path:
    """Return the absolute path to the repo root directory.

    The test file lives at ``aimv/test/test_structure.py``, so we go up three
    levels from ``__file__`` (test → aimv → repo-root).
    """
    return Path(__file__).resolve().parent.parent.parent


# ---------------------------------------------------------------------------
# Directories that must be present (relative to aimv root)
# ---------------------------------------------------------------------------
REQUIRED_DIRS = [
    "aimv/driver",
    "aimv/mcp_server",
    "aimv/test",
    "aimv/benchmarks",
    "aimv/config",
]


# ---------------------------------------------------------------------------
# ``__init__.py`` files that must be present (relative to aimv root)
# ---------------------------------------------------------------------------
REQUIRED_INIT_FILES = [
    "aimv/driver/__init__.py",
    "aimv/mcp_server/__init__.py",
    "aimv/test/__init__.py",
]


# ---------------------------------------------------------------------------
# Other *files* that must exist (relative to aimv root)
# ---------------------------------------------------------------------------
REQUIRED_FILES = [
    "aimv/config/aimv_config.yaml",
    "aimv/driver/requirements.txt",
    "aimv/mcp_server/requirements.txt",
]


class TestDirectoryStructure:
    """Verify every required directory exists on disk."""

    @pytest.mark.parametrize("rel_dir", REQUIRED_DIRS)
    def test_required_directory_exists(self, rel_dir: str) -> None:
        full = _aimv_root() / rel_dir
        assert full.is_dir(), (
            f"Required directory missing: {rel_dir}  "
            f"(resolved to {full})"
        )


class TestInitFiles:
    """Verify every required ``__init__.py`` exists and is importable."""

    @pytest.mark.parametrize("rel_path", REQUIRED_INIT_FILES)
    def test_init_file_exists(self, rel_path: str) -> None:
        full = _aimv_root() / rel_path
        assert full.is_file(), (
            f"Required __init__.py missing: {rel_path}  "
            f"(resolved to {full})"
        )

    @pytest.mark.parametrize("rel_path", REQUIRED_INIT_FILES)
    def test_init_file_has_content(self, rel_path: str) -> None:
        """Each __init__.py must be non-empty — an empty file suggests
        it was created by accident rather than as a deliberate package
        marker."""
        full = _aimv_root() / rel_path
        if not full.is_file():
            pytest.skip(f"File does not exist: {rel_path}")
        content = full.read_text().strip()
        assert content, f"__init__.py is empty: {rel_path}"


class TestRequiredFiles:
    """Verify every required configuration / requirements file exists."""

    @pytest.mark.parametrize("rel_path", REQUIRED_FILES)
    def test_required_file_exists(self, rel_path: str) -> None:
        full = _aimv_root() / rel_path
        assert full.is_file(), (
            f"Required file missing: {rel_path}  (resolved to {full})"
        )
