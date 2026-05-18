# [AIMV] T4.1 — End-to-end integration tests
import json
import tempfile
import pytest
from pathlib import Path
from unittest import mock

from aimv.driver.aimv_driver import main
from aimv.driver.session_store import SessionStore
from aimv.driver.models import (
    SessionRecord, TerminationReason, PerFunctionResult,
)
from aimv.driver.iteration_engine import IterationEngine, NextAction
from aimv.driver.build_orchestrator import BuildResult, VectorizationStatus


ALIAS_FAIL_SRC = """\
void process_task(int *a, int *b, int n) {
    for (int i = 0; i < n; i++) {
        a[i] = b[i] + b[i + 1];
    }
}
"""


@pytest.fixture
def work_dir():
    d = tempfile.mkdtemp(prefix="aimv-e2e-")
    yield d
    import shutil
    shutil.rmtree(d, ignore_errors=True)


class TestE2ECli:
    def test_help_exits_cleanly(self):
        with pytest.raises(SystemExit) as exc_info:
            main(["--help"])
        assert exc_info.value.code == 0

    def test_list_sessions_empty(self, work_dir):
        store = SessionStore(work_dir)
        assert store.list_sessions() == []

    def test_session_roundtrip(self, work_dir):
        store = SessionStore(work_dir)
        s = SessionRecord(source_file="test.c")
        s.termination_reason = TerminationReason.VECTORIZED
        store.save(s)

        loaded = store.load(s.session_id)
        assert loaded is not None
        assert loaded.source_file == "test.c"

        sessions = store.list_sessions()
        assert len(sessions) == 1


class TestIterationEngineIntegration:
    def test_vectorized_terminates_immediately(self):
        engine = IterationEngine("conservative", 5)
        action, _ = engine.decide(1, True, True, True, True, True)
        assert action == NextAction.STOP

    def test_max_rounds_terminates(self):
        engine = IterationEngine("conservative", 3)
        action, _ = engine.decide(3, True, True, False, True, True)
        assert action == NextAction.STOP

    def test_compile_failure_then_success(self):
        engine = IterationEngine("conservative", 5)
        a1, _ = engine.decide(1, False, True, False, True, True, compile_phase="patch")
        assert a1 == NextAction.ROLLBACK
        a2, _ = engine.decide(2, True, True, False, True, True)
        assert a2 == NextAction.CONTINUE

    def test_escalation_chain(self):
        engine = IterationEngine("conservative", 5)
        a1, _ = engine.decide(1, True, True, False, False, True)
        assert a1 == NextAction.ESCALATE_LEVEL
        assert engine.current_level == "moderate"
        a2, _ = engine.decide(1, True, True, False, False, True)
        assert a2 == NextAction.ESCALATE_LEVEL
        assert engine.current_level == "aggressive"
        a3, _ = engine.decide(1, True, True, False, False, True)
        assert a3 == NextAction.STOP
