# [AIMV] AIMV Driver — Atomic source patching + rollback
import hashlib
import shutil
import subprocess
import fcntl
import os
import time
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional, List


@dataclass
class PatchRecord:
    source_file: str
    backup_path: str
    diff_text: str
    original_hash: str
    applied_at: float = field(default_factory=time.time)


class SourceManager:
    def __init__(self, backup_dir: str):
        self.backup_dir = Path(backup_dir)
        self.backup_dir.mkdir(parents=True, exist_ok=True)
        self._patch_history: List[PatchRecord] = []

    def apply_patch(self, source_file: str, diff_text: str) -> PatchRecord:
        src_path = Path(source_file).resolve()
        if not src_path.exists():
            raise FileNotFoundError(f"source file not found: {source_file}")

        original_hash = _sha256(src_path)
        idx = len(self._patch_history)
        backup_path = self.backup_dir / f"{src_path.stem}.r{idx}.bak"
        shutil.copy2(src_path, backup_path)
        diff_path = self.backup_dir / f"{src_path.stem}.r{idx}.diff"
        diff_path.write_text(diff_text, encoding="utf-8")

        proc = subprocess.run(
            ["patch", "-u", "--fuzz=2", str(src_path), str(diff_path)],
            capture_output=True, text=True,
        )
        if proc.returncode != 0:
            shutil.copy2(backup_path, src_path)
            backup_path.unlink(missing_ok=True)
            diff_path.unlink(missing_ok=True)
            raise RuntimeError(f"patch apply failed: {proc.stderr}")

        record = PatchRecord(
            source_file=str(src_path), backup_path=str(backup_path),
            diff_text=diff_text.rstrip(), original_hash=original_hash,
        )
        self._patch_history.append(record)
        diff_path.unlink(missing_ok=True)
        return record

    def rollback(self, patch: PatchRecord) -> bool:
        src_path = Path(patch.source_file)
        backup_path = Path(patch.backup_path)
        if not backup_path.exists():
            raise FileNotFoundError(f"backup not found: {backup_path}")
        backup_hash = _sha256(backup_path)
        if backup_hash != patch.original_hash:
            raise RuntimeError(f"backup hash mismatch")
        shutil.copy2(backup_path, src_path)
        return True

    def rollback_all(self):
        errors = []
        for patch in reversed(self._patch_history):
            try:
                self.rollback(patch)
            except Exception as e:
                errors.append((patch.source_file, str(e)))
        if errors:
            raise RuntimeError(f"rollback_all: {len(errors)} failures: {errors}")

    def get_current_diff(self) -> Optional[str]:
        if self._patch_history:
            return self._patch_history[-1].diff_text
        return None

    def has_diff(self, diff_text: str) -> bool:
        """Check if an identical diff has already been applied."""
        stripped = diff_text.strip()
        return any(p.diff_text.strip() == stripped for p in self._patch_history)


class FileLock:
    def __init__(self, lock_dir: str):
        self.lock_dir = Path(lock_dir)
        self.lock_dir.mkdir(parents=True, exist_ok=True)
        self._locks: dict[str, int] = {}

    def acquire(self, source_file: str, timeout_seconds: int = 30) -> bool:
        path = Path(source_file).resolve()
        lock_name = hashlib.sha256(str(path).encode()).hexdigest()[:16]
        lock_path = self.lock_dir / f"{lock_name}.lock"
        fd = os.open(lock_path, os.O_CREAT | os.O_RDWR)
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

    def release(self, source_file: str):
        path = str(Path(source_file).resolve())
        fd = self._locks.pop(path, None)
        if fd is not None:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()
