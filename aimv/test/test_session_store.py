"""T3.7 — SessionStore persistence tests."""
import sys
import tempfile
import time
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.session_store import (
    SessionStore, SessionRecord, RoundRecord, DriverStatus, TerminationReason
)


@pytest.fixture
def store():
    d = tempfile.mkdtemp(prefix="aimv-test-sessions-")
    yield SessionStore(d)
    import shutil
    shutil.rmtree(d, ignore_errors=True)


class TestSessionStore:
    def test_save_and_load(self, store):
        session = SessionRecord(
            function_name="test_func",
            source_files=["test.c"],
            aimv_level="moderate",
            max_rounds=5,
        )
        session.termination_reason = TerminationReason.VECTORIZED
        store.save(session)
        loaded = store.load(session.session_id)
        assert loaded is not None
        assert loaded.function_name == "test_func"
        assert loaded.termination_reason == TerminationReason.VECTORIZED

    def test_load_nonexistent(self, store):
        assert store.load("nonexistent") is None

    def test_list_sessions(self, store):
        s1 = SessionRecord(function_name="func_a")
        s2 = SessionRecord(function_name="func_b")
        store.save(s1)
        time.sleep(0.01)
        store.save(s2)
        sessions = store.list_sessions()
        assert len(sessions) >= 2

    def test_atomic_write(self, store):
        session = SessionRecord(function_name="test")
        store.save(session)
        # Ensure .tmp file doesn't linger
        tmp_files = list(store.sessions_dir.glob("*.tmp"))
        assert len(tmp_files) == 0

    def test_enum_serialized_as_string(self, store):
        session = SessionRecord(function_name="test")
        session.termination_reason = TerminationReason.TEST_FAILURE
        store.save(session)
        path = store.sessions_dir / f"{session.session_id}.json"
        text = path.read_text()
        assert "test_failure" in text
        assert "TerminationReason" not in text
