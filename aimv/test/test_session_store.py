# [AIMV] T3.5 — SessionStore persistence tests
import re
import time
import pytest
from pathlib import Path

from aimv.driver.session_store import SessionStore
from aimv.driver.models import (
    SessionRecord, PerFunctionResult, RoundRecord, TerminationReason,
    IterationStatus, BuildResult, PatchRecord, VectorizationStatus,
)


@pytest.fixture
def store(tmp_path):
    return SessionStore(str(tmp_path))


class TestSessionStoreSaveLoad:
    def test_save_and_load(self, store):
        """save → load → content matches."""
        session = SessionRecord(
            source_file="test.c",
            aimv_level="conservative",
            max_rounds=5,
        )
        store.save(session)
        loaded = store.load(session.session_id)
        assert loaded is not None
        assert loaded.session_id == session.session_id
        assert loaded.source_file == "test.c"
        assert loaded.aimv_level == "conservative"

    def test_save_and_load_with_functions(self, store):
        """Session with PerFunctionResult roundtrips correctly."""
        session = SessionRecord(source_file="test.c")
        pfr = PerFunctionResult(function_name="foo", vectorized=True, rounds_used=2)
        pfr.termination_reason = TerminationReason.VECTORIZED
        rr = RoundRecord(round_number=1)
        rr.status = IterationStatus.COMPILING
        pfr.rounds.append(rr)
        session.functions.append(pfr)
        store.save(session)

        loaded = store.load(session.session_id)
        assert loaded is not None
        assert len(loaded.functions) == 1
        assert loaded.functions[0].function_name == "foo"
        assert loaded.functions[0].vectorized is True
        assert loaded.functions[0].termination_reason == TerminationReason.VECTORIZED
        assert len(loaded.functions[0].rounds) == 1

    def test_load_nonexistent(self, store):
        """Loading nonexistent session → None."""
        assert store.load("nonexistent") is None


class TestSessionStoreAtomic:
    def test_save_atomic(self, store):
        """No .tmp files linger after save."""
        session = SessionRecord(source_file="test.c")
        store.save(session)
        tmp_files = list(store.sessions_dir.glob("*.tmp"))
        assert len(tmp_files) == 0


class TestSessionStoreList:
    def test_list_sessions(self, store):
        """3 sessions → list length 3."""
        s1 = SessionRecord(source_file="a.c")
        store.save(s1)
        time.sleep(0.01)
        s2 = SessionRecord(source_file="b.c")
        store.save(s2)
        time.sleep(0.01)
        s3 = SessionRecord(source_file="c.c")
        store.save(s3)
        sessions = store.list_sessions()
        assert len(sessions) == 3


class TestSessionIdFormat:
    def test_session_id_format(self):
        """session_id matches aimv-[a-f0-9]{12}."""
        session = SessionRecord()
        assert re.match(r"aimv-[a-f0-9]{12}", session.session_id)

    def test_session_id_unique(self):
        """Two sessions have different IDs."""
        s1 = SessionRecord()
        s2 = SessionRecord()
        assert s1.session_id != s2.session_id


class TestEnumSerialization:
    def test_enum_serialized_as_string(self, store):
        """TerminationReason serializes as its value string."""
        session = SessionRecord(source_file="test.c")
        session.termination_reason = TerminationReason.TEST_FAILURE
        store.save(session)
        path = store.sessions_dir / f"{session.session_id}.json"
        text = path.read_text()
        assert "test_failure" in text
        assert "TerminationReason" not in text
