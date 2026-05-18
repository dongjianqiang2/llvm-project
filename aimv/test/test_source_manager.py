# [AIMV] T3.3 — SourceManager: shadow file protocol + rollback + FileLock + race detection
import hashlib
import tempfile
import threading
import pytest
from pathlib import Path

from aimv.driver.source_manager import SourceManager
from aimv.driver.models import PatchRecord


@pytest.fixture
def tmp_dir(tmp_path):
    return str(tmp_path)


@pytest.fixture
def tmp_source_file(tmp_path):
    f = tmp_path / "test.c"
    f.write_text("int foo(int x) { return x + 1; }\n")
    return str(f)


@pytest.fixture
def sm(tmp_dir):
    return SourceManager(tmp_dir)


class TestShadowPatchAndCommit:
    def test_apply_and_commit_shadow(self, sm, tmp_source_file):
        """cp → patch → commit → original file updated."""
        src = Path(tmp_source_file)
        original = src.read_text()
        diff = (
            f"--- a/{src.name}\n+++ b/{src.name}\n"
            f"@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n"
            f"+int bar(int y) {{ return y * 2; }}\n"
        )
        sm.acquire_lock(tmp_source_file)
        try:
            record = sm.apply_shadow_patch(tmp_source_file, diff)
            assert isinstance(record, PatchRecord)
            # Before commit, original should be unchanged
            assert src.read_text() == original
            # Commit
            assert sm.commit_shadow(tmp_source_file) is True
            # After commit, original should have the change
            assert "bar" in src.read_text()
        finally:
            sm.release_lock(tmp_source_file)

    def test_apply_and_discard_shadow(self, sm, tmp_source_file):
        """cp → patch → discard → original file unchanged."""
        src = Path(tmp_source_file)
        original = src.read_text()
        diff = (
            f"--- a/{src.name}\n+++ b/{src.name}\n"
            f"@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n"
            f"+int bar(int y) {{ return y * 2; }}\n"
        )
        sm.acquire_lock(tmp_source_file)
        try:
            sm.apply_shadow_patch(tmp_source_file, diff)
            sm.discard_shadow(tmp_source_file)
            assert src.read_text() == original
            # Shadow file should be gone
            assert not (src.parent / f"{src.name}.aimv-tmp").exists()
        finally:
            sm.release_lock(tmp_source_file)

    def test_commit_no_shadow_returns_false(self, sm, tmp_source_file):
        """commit_shadow with no .aimv-tmp → returns False."""
        sm.acquire_lock(tmp_source_file)
        try:
            assert sm.commit_shadow(tmp_source_file) is False
        finally:
            sm.release_lock(tmp_source_file)


class TestRollback:
    def test_rollback_restores_original(self, sm, tmp_source_file):
        """2 patches → rollback_all → file restored."""
        src = Path(tmp_source_file)
        original = src.read_text()
        diff1 = (
            f"--- a/{src.name}\n+++ b/{src.name}\n"
            f"@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n"
            f"+int bar(int y) {{ return y * 2; }}\n"
        )
        sm.acquire_lock(tmp_source_file)
        try:
            r1 = sm.apply_shadow_patch(tmp_source_file, diff1)
            sm.commit_shadow(tmp_source_file)
        finally:
            sm.release_lock(tmp_source_file)

        diff2 = (
            f"--- a/{src.name}\n+++ b/{src.name}\n"
            f"@@ -1,2 +1,3 @@\n int foo(int x) {{ return x + 1; }}\n"
            f" int bar(int y) {{ return y * 2; }}\n"
            f"+int baz(int z) {{ return z * 3; }}\n"
        )
        sm.acquire_lock(tmp_source_file)
        try:
            r2 = sm.apply_shadow_patch(tmp_source_file, diff2)
            sm.commit_shadow(tmp_source_file)
        finally:
            sm.release_lock(tmp_source_file)

        # Rollback all
        sm.rollback_all()
        assert src.read_text() == original

    def test_rollback_last_only(self, sm, tmp_source_file):
        """rollback_last only reverts the last patch."""
        src = Path(tmp_source_file)
        diff1 = (
            f"--- a/{src.name}\n+++ b/{src.name}\n"
            f"@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n"
            f"+int bar(int y) {{ return y * 2; }}\n"
        )
        sm.acquire_lock(tmp_source_file)
        try:
            sm.apply_shadow_patch(tmp_source_file, diff1)
            sm.commit_shadow(tmp_source_file)
        finally:
            sm.release_lock(tmp_source_file)

        diff2 = (
            f"--- a/{src.name}\n+++ b/{src.name}\n"
            f"@@ -1,2 +1,3 @@\n int foo(int x) {{ return x + 1; }}\n"
            f" int bar(int y) {{ return y * 2; }}\n"
            f"+int baz(int z) {{ return z * 3; }}\n"
        )
        sm.acquire_lock(tmp_source_file)
        try:
            sm.apply_shadow_patch(tmp_source_file, diff2)
            sm.commit_shadow(tmp_source_file)
        finally:
            sm.release_lock(tmp_source_file)

        sm.rollback_last(tmp_source_file)
        text = src.read_text()
        assert "bar" in text
        assert "baz" not in text

    def test_rollback_nonexistent_backup(self, sm, tmp_source_file):
        """Rollback with nonexistent backup → FileNotFoundError."""
        fake = PatchRecord(
            source_file=tmp_source_file,
            backup_path="/nonexistent/backup.bak",
            diff_text="", original_hash="abc",
        )
        with pytest.raises(FileNotFoundError):
            sm.rollback(fake)


class TestFileLock:
    def test_file_lock_serialization(self, sm, tmp_source_file):
        """Two threads competing for same file lock → serialized."""
        results = []

        def worker():
            ok = sm.acquire_lock(tmp_source_file, timeout_seconds=5)
            results.append(ok)
            import time
            time.sleep(0.1)  # hold lock briefly
            sm.release_lock(tmp_source_file)

        t1 = threading.Thread(target=worker)
        t2 = threading.Thread(target=worker)
        t1.start()
        t2.start()
        t1.join(timeout=10)
        t2.join(timeout=10)
        assert len(results) == 2
        assert all(results)

    def test_file_lock_timeout(self, sm, tmp_source_file):
        """Lock acquisition timeout → returns False."""
        # Acquire lock in main thread
        assert sm.acquire_lock(tmp_source_file, timeout_seconds=1) is True
        # Another attempt with short timeout should fail
        assert sm.acquire_lock(tmp_source_file, timeout_seconds=0) is False
        sm.release_lock(tmp_source_file)


class TestRaceDetection:
    def test_race_detection_stale(self, sm, tmp_source_file):
        """File modified after MCP query → check_stale() returns True."""
        pre_hash = sm.snapshot_hash(tmp_source_file)
        # Modify the file
        Path(tmp_source_file).write_text("int modified(void) { return 0; }\n")
        assert sm.check_stale(tmp_source_file, pre_hash) is True

    def test_race_detection_fresh(self, sm, tmp_source_file):
        """File unchanged after MCP query → check_stale() returns False."""
        pre_hash = sm.snapshot_hash(tmp_source_file)
        assert sm.check_stale(tmp_source_file, pre_hash) is False


class TestStaleShadowDetection:
    def test_stale_shadow_detection(self, sm, tmp_source_file):
        """Residual .aimv-tmp → check_stale_shadow() returns path."""
        src = Path(tmp_source_file)
        shadow = src.parent / f"{src.name}.aimv-tmp"
        shadow.write_text("stale content")
        result = sm.check_stale_shadow(tmp_source_file)
        assert result is not None
        assert result == str(shadow)

    def test_no_stale_shadow(self, sm, tmp_source_file):
        """No .aimv-tmp → check_stale_shadow() returns None."""
        assert sm.check_stale_shadow(tmp_source_file) is None


class TestHashIntegrity:
    def test_hash_integrity(self, sm, tmp_source_file):
        """Patch → commit → file hash matches expected."""
        src = Path(tmp_source_file)
        diff = (
            f"--- a/{src.name}\n+++ b/{src.name}\n"
            f"@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n"
            f"+int bar(int y) {{ return y * 2; }}\n"
        )
        sm.acquire_lock(tmp_source_file)
        try:
            sm.apply_shadow_patch(tmp_source_file, diff)
            sm.commit_shadow(tmp_source_file)
        finally:
            sm.release_lock(tmp_source_file)

        content = src.read_bytes()
        expected_hash = hashlib.sha256(content).hexdigest()
        assert sm.file_hash(tmp_source_file) == expected_hash


class TestHasDiff:
    def test_has_diff_detects_duplicate(self, sm, tmp_source_file):
        """has_diff() returns True for already-applied diff."""
        src = Path(tmp_source_file)
        diff = (
            f"--- a/{src.name}\n+++ b/{src.name}\n"
            f"@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n"
            f"+int bar(int y) {{ return y * 2; }}\n"
        )
        sm.acquire_lock(tmp_source_file)
        try:
            sm.apply_shadow_patch(tmp_source_file, diff)
            sm.commit_shadow(tmp_source_file)
        finally:
            sm.release_lock(tmp_source_file)

        assert sm.has_diff(diff) is True

    def test_has_diff_new(self, sm, tmp_source_file):
        """has_diff() returns False for new diff."""
        assert sm.has_diff("--- a/test.c\n+++ b/test.c\n@@\n-new\n+new\n") is False
