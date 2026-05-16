"""T3.4 — SourceManager: atomic patch + rollback tests."""
import sys
import hashlib
import tempfile
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.source_manager import SourceManager, PatchRecord


@pytest.fixture
def tmp_file():
    with tempfile.NamedTemporaryFile(mode="w", suffix=".c", delete=False) as f:
        f.write("int foo(int x) { return x + 1; }\n")
        path = Path(f.name)
    yield str(path)
    path.unlink(missing_ok=True)
    for bak in path.parent.glob(f"{path.stem}.*.bak"):
        bak.unlink(missing_ok=True)
    for diff in path.parent.glob(f"{path.stem}.*.diff"):
        diff.unlink(missing_ok=True)


@pytest.fixture
def backup_dir():
    d = tempfile.mkdtemp(prefix="aimv-test-backups-")
    yield d
    import shutil
    shutil.rmtree(d, ignore_errors=True)


class TestSourceManagerApplyPatch:
    def test_apply_adds_line(self, tmp_file, backup_dir):
        sm = SourceManager(backup_dir)
        original = Path(tmp_file).read_text()
        diff = f"--- a/{Path(tmp_file).name}\n+++ b/{Path(tmp_file).name}\n@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n+int bar(int y) {{ return y * 2; }}\n"
        record = sm.apply_patch(tmp_file, diff)
        assert isinstance(record, PatchRecord)
        new_text = Path(tmp_file).read_text()
        assert "bar" in new_text

    def test_apply_rollback_restores(self, tmp_file, backup_dir):
        sm = SourceManager(backup_dir)
        original = Path(tmp_file).read_text()
        diff = f"--- a/{Path(tmp_file).name}\n+++ b/{Path(tmp_file).name}\n@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n+int bar(int y) {{ return y * 2; }}\n"
        record = sm.apply_patch(tmp_file, diff)
        assert sm.rollback(record)
        assert Path(tmp_file).read_text() == original

    def test_bad_diff_raises_and_restores(self, tmp_file, backup_dir):
        sm = SourceManager(backup_dir)
        original = Path(tmp_file).read_text()
        with pytest.raises(RuntimeError, match="patch apply failed"):
            sm.apply_patch(tmp_file, "this is not a valid diff")
        assert Path(tmp_file).read_text() == original

    def test_multiple_patches_rollback_all(self, tmp_file, backup_dir):
        sm = SourceManager(backup_dir)
        original = Path(tmp_file).read_text()
        diff1 = f"--- a/{Path(tmp_file).name}\n+++ b/{Path(tmp_file).name}\n@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n+int bar(int y) {{ return y * 2; }}\n"
        diff2 = f"--- a/{Path(tmp_file).name}\n+++ b/{Path(tmp_file).name}\n@@ -1,2 +1,3 @@\n int foo(int x) {{ return x + 1; }}\n int bar(int y) {{ return y * 2; }}\n+int baz(int z) {{ return z * 3; }}\n"
        sm.apply_patch(tmp_file, diff1)
        sm.apply_patch(tmp_file, diff2)
        sm.rollback_all()
        assert Path(tmp_file).read_text() == original

    def test_get_current_diff(self, tmp_file, backup_dir):
        sm = SourceManager(backup_dir)
        diff = f"--- a/{Path(tmp_file).name}\n+++ b/{Path(tmp_file).name}\n@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n+int bar(int y) {{ return y * 2; }}\n"
        sm.apply_patch(tmp_file, diff)
        current = sm.get_current_diff()
        assert "bar" in current


class TestSourceManagerRollback:
    def test_rollback_nonexistent_backup(self, tmp_file, backup_dir):
        sm = SourceManager(backup_dir)
        fake = PatchRecord(source_file=tmp_file,
                           backup_path="/nonexistent/backup.bak",
                           diff_text="", original_hash="abc")
        with pytest.raises(FileNotFoundError):
            sm.rollback(fake)

    def test_rollback_hash_mismatch(self, tmp_file, backup_dir):
        sm = SourceManager(backup_dir)
        # Create a valid backup first
        original = Path(tmp_file).read_text()
        diff = f"--- a/{Path(tmp_file).name}\n+++ b/{Path(tmp_file).name}\n@@ -1 +1,2 @@\n int foo(int x) {{ return x + 1; }}\n+int bar(int y) {{ return y * 2; }}\n"
        record = sm.apply_patch(tmp_file, diff)
        # Tamper with the hash
        record.original_hash = "deadbeef"
        with pytest.raises(RuntimeError, match="hash mismatch"):
            sm.rollback(record)
