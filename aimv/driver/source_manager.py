# [AIMV] AIMV Driver — Atomic source patching + rollback + shadow file protocol + FileLock
import hashlib
import shutil
import subprocess
import fcntl
import os
import time
from pathlib import Path
from typing import Optional, List

from .models import PatchRecord


class SourceManager:
    """Source code shadow file patch, atomic replace, rollback and FileLock management.

    Shadow file protocol (SPEC §3.1):
      1. [no lock] Record current file hash (pre_query_hash)
      2. [no lock] MCP query → get AI suggestion
      3. [acquire lock]
         a. Verify file hash matches pre_query_hash
         b. cp source → source.aimv-tmp
         c. patch source.aimv-tmp
         d. clang -c source.aimv-tmp (compile verify)
         e. pass → mv source.aimv-tmp source (atomic rename)
            fail → rm source.aimv-tmp
      4. [release lock]
    """

    def __init__(self, output_dir: str):
        self.backup_dir = Path(output_dir) / "backups"
        self.backup_dir.mkdir(parents=True, exist_ok=True)
        self.lock_dir = Path(output_dir) / "locks"
        self.lock_dir.mkdir(parents=True, exist_ok=True)
        self._patch_history: List[PatchRecord] = []
        self._locks: dict = {}  # path → fd

    # ── Race protection ──────────────────────────────────────────

    @staticmethod
    def file_hash(path: str) -> str:
        """Compute file SHA256 for race detection."""
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(8192), b""):
                h.update(chunk)
        return h.hexdigest()

    def snapshot_hash(self, source_file: str) -> str:
        """Record file hash before MCP query."""
        return self.file_hash(source_file)

    def check_stale(self, source_file: str, pre_query_hash: str) -> bool:
        """Check if file was modified during lock-free MCP query period."""
        current_hash = self.file_hash(source_file)
        return current_hash != pre_query_hash

    # ── FileLock ──────────────────────────────────────────

    def acquire_lock(self, source_file: str, timeout_seconds: int = 30) -> bool:
        """Acquire per-source-file file lock."""
        path = Path(source_file).resolve()
        lock_name = hashlib.sha256(str(path).encode()).hexdigest()[:16]
        lock_path = self.lock_dir / f"{lock_name}.lock"

        fd = os.open(str(lock_path), os.O_CREAT | os.O_RDWR)
        deadline = time.time() + timeout_seconds

        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self._locks[str(path)] = fd
                return True
            except BlockingIOError:
                if time.time() > deadline:
                    os.close(fd)
                    return False
                time.sleep(0.5)

    def release_lock(self, source_file: str):
        """Release file lock."""
        path = str(Path(source_file).resolve())
        fd = self._locks.pop(path, None)
        if fd is not None:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)

    # ── Shadow file protocol ─────────────────────────────────────

    def apply_shadow_patch(self, source_file: str, diff_text: str) -> PatchRecord:
        """Shadow file patch protocol (SPEC §3.1).

        Caller must acquire_lock() before calling this.
        Steps:
          a. cp source → source.aimv-tmp
          b. write diff temp file
          c. patch source.aimv-tmp
          d. return PatchRecord
        """
        src_path = Path(source_file).resolve()
        if not src_path.exists():
            raise FileNotFoundError(f"source file not found: {source_file}")

        original_hash = _sha256(src_path)

        # Step a: cp source → source.aimv-tmp
        shadow_path = src_path.parent / f"{src_path.name}.aimv-tmp"
        shutil.copy2(src_path, shadow_path)

        # Create backup (for rollback)
        backup_path = self.backup_dir / f"{src_path.stem}.r{len(self._patch_history)}.bak"
        shutil.copy2(src_path, backup_path)

        # Step b: write diff temp file
        diff_path = self.backup_dir / f"{src_path.stem}.r{len(self._patch_history)}.diff"
        diff_path.write_text(diff_text, encoding="utf-8")

        # Step c: patch source.aimv-tmp
        proc = subprocess.run(
            ["patch", "-u", "--fuzz=2", str(shadow_path), str(diff_path)],
            capture_output=True, text=True,
        )
        diff_path.unlink(missing_ok=True)

        if proc.returncode != 0:
            shadow_path.unlink(missing_ok=True)
            backup_path.unlink(missing_ok=True)
            raise RuntimeError(f"patch apply failed: {proc.stderr}")

        record = PatchRecord(
            source_file=str(src_path),
            backup_path=str(backup_path),
            diff_text=diff_text.rstrip(),
            original_hash=original_hash,
        )
        self._patch_history.append(record)
        return record

    def commit_shadow(self, source_file: str) -> bool:
        """Shadow file verified → atomic mv replace original."""
        src_path = Path(source_file).resolve()
        shadow_path = src_path.parent / f"{src_path.name}.aimv-tmp"

        if not shadow_path.exists():
            return False

        os.replace(str(shadow_path), str(src_path))  # atomic rename(2)
        return True

    def discard_shadow(self, source_file: str):
        """Shadow file verification failed → rm shadow."""
        src_path = Path(source_file).resolve()
        shadow_path = src_path.parent / f"{src_path.name}.aimv-tmp"
        shadow_path.unlink(missing_ok=True)

    def rollback(self, patch: PatchRecord) -> bool:
        """Restore original file from backup."""
        src_path = Path(patch.source_file)
        backup_path = Path(patch.backup_path)

        if not backup_path.exists():
            raise FileNotFoundError(f"backup not found: {backup_path}")

        backup_hash = _sha256(backup_path)
        if backup_hash != patch.original_hash:
            raise RuntimeError(
                f"backup hash mismatch: expected {patch.original_hash}, got {backup_hash}"
            )

        shutil.copy2(backup_path, src_path)
        return True

    def rollback_all(self):
        """Roll back all patches in reverse order."""
        errors = []
        for patch in reversed(self._patch_history):
            try:
                self.rollback(patch)
            except Exception as e:
                errors.append((patch.source_file, str(e)))
        if errors:
            raise RuntimeError(f"rollback_all: {len(errors)} failures: {errors}")

    def rollback_last(self, source_file: str):
        """Roll back only the last patch for a given source file."""
        for patch in reversed(self._patch_history):
            if patch.source_file == str(Path(source_file).resolve()):
                self.rollback(patch)
                self._patch_history.remove(patch)
                return

    # ── Stale shadow detection ─────────────────────────────────────

    def check_stale_shadow(self, source_file: str) -> Optional[str]:
        """Detect stale shadow file (from kill -9). Returns path or None."""
        src_path = Path(source_file).resolve()
        shadow_path = src_path.parent / f"{src_path.name}.aimv-tmp"
        if shadow_path.exists():
            return str(shadow_path)
        return None

    def warn_stale_shadow(self, source_file: str):
        """Warn about stale shadow file on startup."""
        import sys
        stale = self.check_stale_shadow(source_file)
        if stale:
            print(
                f"[AIMV] WARNING: stale shadow file detected: {stale}\n"
                f"  This may be from a previous aimv-driver process killed by signal.\n"
                f"  Please inspect the file manually before proceeding.\n"
                f"  To discard: rm {stale}",
                file=sys.stderr,
            )

    # ── Cumulative patch generation ──────────────────────────────────

    def generate_cumulative_patch(self, source_file: str, pristine_dir: str) -> Optional[str]:
        """Generate cumulative unified diff (relative to pristine source)."""
        src_path = Path(source_file).resolve()
        pristine = Path(pristine_dir) / src_path.name
        if not pristine.exists():
            return None

        proc = subprocess.run(
            ["diff", "-u", str(pristine), str(src_path)],
            capture_output=True, text=True,
        )
        if proc.returncode == 1:
            patch_path = src_path.parent / f"{src_path.name}.aimv.patch"
            patch_path.write_text(proc.stdout, encoding="utf-8")
            return str(patch_path)
        return None

    def has_diff(self, diff_text: str) -> bool:
        """Check if an identical diff has already been applied."""
        stripped = diff_text.strip()
        return any(p.diff_text.strip() == stripped for p in self._patch_history)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()
