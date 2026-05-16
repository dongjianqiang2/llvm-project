"""T4.1 — End-to-end integration tests.

Tests full AIMV pipeline:
  compile → diagnose → MCP query → apply patch → recompile → verify
CLI and session store are tested directly; the full loop is smoke-tested
with mocks to verify the iteration engine integration.
"""
import sys
import json
import tempfile
import pytest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.aimv_driver import main, main_loop
from aimv.driver.session_store import (
    SessionStore, SessionRecord, TerminationReason,
)
from aimv.driver.iteration_engine import IterationEngine, NextAction
from aimv.driver.build_orchestrator import BuildResult, VectorizationStatus


# Minimal C source with a loop that won't vectorize
ALIAS_FAIL_SRC = """\
void process_task(int *a, int *b, int n) {
    for (int i = 0; i < n; i++) {
        a[i] = b[i] + b[i + 1];
    }
}
"""

MOCK_MCP_RESPONSE = {
    "request_id": "test",
    "suggestions": [{
        "description": "Add restrict qualifier",
        "reasoning": "Alias analysis failed",
        "source_file": "{source_file}",
        "line_start": 1, "line_end": 1,
        "original": "void process_task(int *a, int *b, int n) {",
        "modified": "void process_task(int * restrict a, int *b, int n) {",
        "diff": "--- a/test.c\n+++ b/test.c\n@@ -1,1 +1,1 @@\n-void process_task(int *a, int *b, int n) {\n+void process_task(int * restrict a, int *b, int n) {",
        "estimated_impact": "high",
    }],
    "overall_analysis": "Add restrict to a",
    "confidence": 0.92,
    "no_action_possible": False,
}

MOCK_MCP_NO_SUGGESTION = {
    "request_id": "test", "suggestions": [],
    "overall_analysis": "Cannot fix", "confidence": 1.0,
    "no_action_possible": True,
}


@pytest.fixture
def work_dir():
    d = tempfile.mkdtemp(prefix="aimv-e2e-")
    yield d
    import shutil
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture
def source_file(work_dir):
    path = Path(work_dir) / "test.c"
    path.write_text(ALIAS_FAIL_SRC)
    return str(path)


class TestFullPipeline:
    """Smoke test: full main_loop with all external deps mocked."""

    @mock.patch("aimv.driver.mcp_client.MCPClient.analyze")
    @mock.patch("aimv.driver.mcp_client.MCPClient.health")
    @mock.patch("aimv.driver.aimv_driver.BuildOrchestrator.compile_with_aimv")
    @mock.patch("aimv.driver.aimv_driver.BuildOrchestrator.check_vectorization_from_json")
    def test_loop_terminates_no_suggestion(
        self, mock_check, mock_compile, mock_health, mock_analyze,
        source_file, work_dir,
    ):
        """MCP returns no_action_possible → main_loop returns non-zero."""
        mock_health.return_value = True
        mock_analyze.return_value = MOCK_MCP_NO_SUGGESTION

        mock_compile.return_value = BuildResult(
            returncode=0, stdout="", stderr="",
            opt_record_path=f"{work_dir}/opt.yaml",
            aimv_json_path=f"{work_dir}/aimv.json",
            elapsed_ms=100,
        )
        # Write mock AIMV JSON
        with open(f"{work_dir}/aimv.json", 'w') as f:
            json.dump({
                "request_id": "test",
                "target": {"triple": "arm", "cpu": "", "features": [], "vector_width": 128},
                "diagnostics": [{
                    "pass_name": "LoopVectorize", "remark_id": "CantReorderMemOps",
                    "remark_text": "unsafe memory operations",
                    "severity": "missed", "function_name": "process_task",
                    "loop_location": "test.c:2:5", "source_context": "", "ir_snippet": "",
                }],
            }, f)

        mock_check.return_value = VectorizationStatus(
            "process_task", 1, 0, 1, [{"loop_location": "test.c:2:5"}])

        cfg = {
            "function": "process_task", "source_file": source_file,
            "aimv_level": "moderate", "max_rounds": 3,
            "mcp_url": "http://localhost:8080", "output_dir": work_dir,
            "backup_dir": f"{work_dir}/backups", "test_cmd": "",
            "output_binary": f"{work_dir}/test.o",
        }
        result = main_loop(cfg)
        assert result != 0  # not VECTORIZED

    @mock.patch("aimv.driver.mcp_client.MCPClient.analyze")
    @mock.patch("aimv.driver.mcp_client.MCPClient.health")
    @mock.patch("aimv.driver.aimv_driver.BuildOrchestrator.compile_with_aimv")
    @mock.patch("aimv.driver.aimv_driver.BuildOrchestrator.check_vectorization_from_json")
    def test_loop_terminates_vectorized(
        self, mock_check, mock_compile, mock_health, mock_analyze,
        source_file, work_dir,
    ):
        """First check shows all passed → main_loop returns 0 immediately."""
        mock_health.return_value = True
        mock_compile.return_value = BuildResult(
            returncode=0, stdout="", stderr="",
            opt_record_path=f"{work_dir}/opt.yaml",
            aimv_json_path=f"{work_dir}/aimv.json",
            elapsed_ms=100,
        )
        # Write mock AIMV JSON with passed diagnostic
        with open(f"{work_dir}/aimv.json", 'w') as f:
            json.dump({
                "request_id": "test",
                "target": {"triple": "arm", "cpu": "", "features": [], "vector_width": 128},
                "diagnostics": [{
                    "pass_name": "LoopVectorize", "remark_id": "LoopVectorized",
                    "remark_text": "loop vectorized", "severity": "passed",
                    "function_name": "process_task", "loop_location": "test.c:2:5",
                    "source_context": "", "ir_snippet": "",
                }],
            }, f)

        mock_check.return_value = VectorizationStatus(
            "process_task", 1, 1, 0, [])
        mock_analyze.return_value = MOCK_MCP_NO_SUGGESTION  # shouldn't be called

        cfg = {
            "function": "process_task", "source_file": source_file,
            "aimv_level": "moderate", "max_rounds": 3,
            "mcp_url": "http://localhost:8080", "output_dir": work_dir,
            "backup_dir": f"{work_dir}/backups", "test_cmd": "",
            "output_binary": f"{work_dir}/test.o",
        }
        result = main_loop(cfg)
        assert result == 0  # VECTORIZED
        mock_analyze.assert_not_called()


class TestE2ECli:
    def test_help_exits_cleanly(self):
        with pytest.raises(SystemExit) as exc_info:
            main(["--help"])
        assert exc_info.value.code == 0

    def test_list_sessions_empty(self, work_dir):
        store = SessionStore(f"{work_dir}/sessions")
        assert store.list_sessions() == []

    def test_session_roundtrip(self, work_dir):
        store = SessionStore(f"{work_dir}/sessions")
        s = SessionRecord(function_name="test_func", source_files=["test.c"])
        s.termination_reason = TerminationReason.VECTORIZED
        s.rounds = []  # 0 rounds
        store.save(s)

        loaded = store.load(s.session_id)
        assert loaded is not None
        assert loaded.function_name == "test_func"

        sessions = store.list_sessions()
        assert len(sessions) == 1
        assert sessions[0]["status"] == "vectorized"


class TestIterationEngineIntegration:
    """Verify iteration engine decision matrix in the context of a full loop."""

    def test_vectorized_terminates_immediately(self):
        engine = IterationEngine("moderate", 5)
        action, _ = engine.decide(1, True, True, True, True, True)
        assert action == NextAction.STOP

    def test_max_rounds_terminates(self):
        engine = IterationEngine("moderate", 3)
        action, _ = engine.decide(3, True, True, False, True, True)
        assert action == NextAction.STOP

    def test_compile_failure_then_success(self):
        engine = IterationEngine("moderate", 5)
        # First: compile failure → rollback + retry
        a1, _ = engine.decide(1, False, True, False, True, True)
        assert a1 == NextAction.ROLLBACK
        # Second round: success
        a2, _ = engine.decide(2, True, True, False, True, True)
        assert a2 == NextAction.CONTINUE

    def test_escalation_chain(self):
        engine = IterationEngine("conservative", 5)
        # No suggestion → escalate to moderate
        a1, _ = engine.decide(1, True, True, False, False, True)
        assert a1 == NextAction.ESCALATE_LEVEL
        assert engine.current_level == "moderate"
        # No suggestion → escalate to aggressive
        a2, _ = engine.decide(1, True, True, False, False, True)
        assert a2 == NextAction.ESCALATE_LEVEL
        assert engine.current_level == "aggressive"
        # No suggestion at aggressive → stop
        a3, _ = engine.decide(1, True, True, False, False, True)
        assert a3 == NextAction.STOP
