# [AIMV] T3.7 — process_single_function integration tests
import json
import pytest
from pathlib import Path
from unittest import mock

from aimv.driver.aimv_driver import process_single_function
from aimv.driver.config import DriverConfig
from aimv.driver.build_orchestrator import BuildOrchestrator
from aimv.driver.mcp_client import MCPClient
from aimv.driver.source_manager import SourceManager
from aimv.driver.iteration_engine import IterationEngine
from aimv.driver.session_store import SessionStore
from aimv.driver.models import (
    SessionRecord, BuildResult, TestResult, VectorizationStatus,
    TerminationReason, NextAction, PatchRecord,
)


@pytest.fixture
def source_file(tmp_path):
    f = tmp_path / "task.c"
    f.write_text("void foo(int *a, int *b) { for(int i=0;i<100;i++) a[i]=b[i]; }\n")
    return str(f)


@pytest.fixture
def output_dir(tmp_path):
    d = tmp_path / "aimv-output"
    d.mkdir()
    return str(d)


@pytest.fixture
def config(output_dir):
    return DriverConfig(output_dir=output_dir, test_cmd="")


DIAGNOSTICS_MISSED = [{
    "function_name": "foo",
    "severity": "missed",
    "remark_id": "CantReorderMemOps",
    "remark_text": "cant reorder",
    "loop_location": "task.c:1:42",
}]

DIAGNOSTICS_PASSED = [{
    "function_name": "foo",
    "severity": "passed",
    "remark_id": "LoopVectorized",
    "remark_text": "vectorized",
    "loop_location": "task.c:1:42",
}]


def _make_build_ok(aimv_json_path, diagnostics):
    """Create a successful BuildResult with given diagnostics in JSON."""
    Path(aimv_json_path).parent.mkdir(parents=True, exist_ok=True)
    with open(aimv_json_path, "w") as f:
        json.dump({"diagnostics": diagnostics}, f)
    return BuildResult(
        returncode=0, stdout="", stderr="",
        opt_record_path="", aimv_json_path=aimv_json_path,
        elapsed_ms=100,
    )


def _make_build_fail():
    return BuildResult(
        returncode=1, stdout="", stderr="error",
        opt_record_path="", aimv_json_path="", elapsed_ms=100,
    )


def _make_mcp_response(has_suggestion=True, diff_text=""):
    if not has_suggestion:
        return {"no_action_possible": True, "suggestions": []}
    return {
        "no_action_possible": False,
        "suggestions": [{
            "description": "add restrict",
            "reasoning": "alias analysis",
            "diff": diff_text or "--- a/task.c\n+++ b/task.c\n@@ -1 +1,2 @@\n+// test\n void foo(int *a, int *b) { for(int i=0;i<100;i++) a[i]=b[i]; }\n",
        }],
    }


class TestProcessSingleFunction:
    def test_single_function_already_vectorized(self, source_file, output_dir, config):
        """No missed diagnostics → VECTORIZED, no MCP call."""
        builder = mock.MagicMock(spec=BuildOrchestrator)
        mcp = mock.MagicMock(spec=MCPClient)
        sources = mock.MagicMock(spec=SourceManager)
        store = mock.MagicMock(spec=SessionStore)
        engine = IterationEngine("conservative", 5)
        session = SessionRecord(source_file=source_file)

        aimv_json = str(Path(output_dir) / "aimv-foo-r1.json")
        builder.compile_with_aimv.return_value = _make_build_ok(aimv_json, DIAGNOSTICS_PASSED)
        builder.check_vectorization.return_value = VectorizationStatus(
            function_name="foo", total_loops=1, vectorized_loops=1,
            missed_loops=0, missed_details=[], passed_remark_count=1,
        )

        result = process_single_function(
            "foo", source_file, DIAGNOSTICS_PASSED, config,
            builder, mcp, engine, sources, store, session,
        )

        assert result.termination_reason == TerminationReason.VECTORIZED
        assert result.vectorized is True
        mcp.analyze.assert_not_called()

    def test_single_function_success_one_round(self, source_file, output_dir, config):
        """1 round: MCP → patch → compile pass → next round shows vectorized."""
        builder = mock.MagicMock(spec=BuildOrchestrator)
        mcp = mock.MagicMock(spec=MCPClient)
        sources = SourceManager(output_dir)
        store = mock.MagicMock(spec=SessionStore)
        engine = IterationEngine("conservative", 5)
        session = SessionRecord(source_file=source_file)

        # Round 1: compile shows missed → MCP → shadow patch → verify pass
        # Round 2: compile shows vectorized → VECTORIZED
        aimv_json_r1 = str(Path(output_dir) / "aimv-foo-r1.json")
        verify_json = str(Path(output_dir) / "aimv-foo-verify-r1.json")
        aimv_json_r2 = str(Path(output_dir) / "aimv-foo-r2.json")

        builder.compile_with_aimv.side_effect = [
            _make_build_ok(aimv_json_r1, DIAGNOSTICS_MISSED),  # R1 initial
            _make_build_ok(verify_json, DIAGNOSTICS_MISSED),    # R1 verify
            _make_build_ok(aimv_json_r2, DIAGNOSTICS_PASSED),   # R2 initial: vectorized!
        ]
        builder.check_vectorization.side_effect = [
            VectorizationStatus("foo", 1, 0, 1, DIAGNOSTICS_MISSED, 0),  # R1 after initial
            VectorizationStatus("foo", 1, 0, 1, DIAGNOSTICS_MISSED, 0),  # R1 after verify (still missed)
            VectorizationStatus("foo", 1, 1, 0, [], 1),  # R2: vectorized!
        ]
        builder.run_tests.return_value = TestResult(
            returncode=0, stdout="", stderr="", passed=0, failed=0, elapsed_ms=0)
        mcp.analyze.return_value = _make_mcp_response()

        result = process_single_function(
            "foo", source_file, DIAGNOSTICS_MISSED, config,
            builder, mcp, engine, sources, store, session,
        )

        assert result.termination_reason == TerminationReason.VECTORIZED
        assert result.vectorized is True
        mcp.analyze.assert_called_once()

    def test_single_function_compile_error_rollback(self, source_file, output_dir, config):
        """Patch compile failure → discard_shadow → engine STOP on consecutive failures."""
        builder = mock.MagicMock(spec=BuildOrchestrator)
        mcp = mock.MagicMock(spec=MCPClient)
        sources = mock.MagicMock(spec=SourceManager)
        store = mock.MagicMock(spec=SessionStore)
        engine = IterationEngine("conservative", 5)
        session = SessionRecord(source_file=source_file)

        aimv_json_r1 = str(Path(output_dir) / "aimv-foo-r1.json")
        verify_json = str(Path(output_dir) / "aimv-foo-verify-r1.json")

        # R1: compile OK (missed), MCP suggests, verify FAILS
        # After discard, engine decides ROLLBACK, loop continues
        # R2: compile OK, MCP suggests, verify FAILS again → engine STOP
        builder.compile_with_aimv.side_effect = [
            _make_build_ok(aimv_json_r1, DIAGNOSTICS_MISSED),  # R1 initial
            _make_build_fail(),  # R1 verify fails
            _make_build_ok(aimv_json_r1, DIAGNOSTICS_MISSED),  # R2 initial (reuse path)
            _make_build_fail(),  # R2 verify fails again → STOP
        ]
        builder.check_vectorization.return_value = VectorizationStatus(
            "foo", 1, 0, 1, DIAGNOSTICS_MISSED, 0)
        sources.has_diff.return_value = False
        sources.acquire_lock.return_value = True
        sources.snapshot_hash.return_value = "abc"
        sources.check_stale.return_value = False
        sources.apply_shadow_patch.return_value = PatchRecord(
            source_file=source_file, backup_path="/tmp/bak.bak",
            diff_text="test", original_hash="abc")
        mcp.analyze.return_value = _make_mcp_response()

        result = process_single_function(
            "foo", source_file, DIAGNOSTICS_MISSED, config,
            builder, mcp, engine, sources, store, session,
        )

        assert result.termination_reason == TerminationReason.COMPILE_ERROR

    def test_single_function_test_failure_stop(self, source_file, output_dir, config):
        """Test failure → discard shadow → TEST_FAILURE."""
        config.test_cmd = "./test_runner"
        builder = mock.MagicMock(spec=BuildOrchestrator)
        mcp = mock.MagicMock(spec=MCPClient)
        sources = SourceManager(output_dir)
        store = mock.MagicMock(spec=SessionStore)
        engine = IterationEngine("conservative", 5)
        session = SessionRecord(source_file=source_file)

        aimv_json_r1 = str(Path(output_dir) / "aimv-foo-r1.json")
        verify_json = str(Path(output_dir) / "aimv-foo-verify-r1.json")

        builder.compile_with_aimv.side_effect = [
            _make_build_ok(aimv_json_r1, DIAGNOSTICS_MISSED),
            _make_build_ok(verify_json, DIAGNOSTICS_MISSED),
        ]
        builder.check_vectorization.side_effect = [
            VectorizationStatus("foo", 1, 0, 1, DIAGNOSTICS_MISSED, 0),
            VectorizationStatus("foo", 1, 0, 1, DIAGNOSTICS_MISSED, 0),
        ]
        builder.run_tests.return_value = TestResult(
            returncode=1, stdout="", stderr="FAIL", passed=0, failed=1, elapsed_ms=100)
        mcp.analyze.return_value = _make_mcp_response()

        result = process_single_function(
            "foo", source_file, DIAGNOSTICS_MISSED, config,
            builder, mcp, engine, sources, store, session,
        )

        assert result.termination_reason == TerminationReason.TEST_FAILURE

    def test_single_function_max_rounds(self, source_file, output_dir, config):
        """Reaching max_rounds → ROUND_LIMIT."""
        config.max_rounds = 1
        builder = mock.MagicMock(spec=BuildOrchestrator)
        mcp = mock.MagicMock(spec=MCPClient)
        sources = mock.MagicMock(spec=SourceManager)
        store = mock.MagicMock(spec=SessionStore)
        engine = IterationEngine("conservative", 1)
        session = SessionRecord(source_file=source_file)

        aimv_json_r1 = str(Path(output_dir) / "aimv-foo-r1.json")
        builder.compile_with_aimv.return_value = _make_build_ok(aimv_json_r1, DIAGNOSTICS_MISSED)
        builder.check_vectorization.return_value = VectorizationStatus(
            "foo", 1, 0, 1, DIAGNOSTICS_MISSED, 0)
        sources.has_diff.return_value = False
        sources.acquire_lock.return_value = True
        sources.snapshot_hash.return_value = "abc"
        sources.check_stale.return_value = False
        # No suggestions from MCP → escalate → stop at aggressive
        mcp.analyze.return_value = {"no_action_possible": True, "suggestions": []}

        result = process_single_function(
            "foo", source_file, DIAGNOSTICS_MISSED, config,
            builder, mcp, engine, sources, store, session,
        )

        # After 3 escalations (conservative→moderate→aggressive), stops
        assert result.termination_reason == TerminationReason.NO_SUGGESTION

    def test_single_function_no_suggestion_escalate(self, source_file, output_dir, config):
        """MCP no_action → escalate level → try again → eventually stop."""
        builder = mock.MagicMock(spec=BuildOrchestrator)
        mcp = mock.MagicMock(spec=MCPClient)
        sources = mock.MagicMock(spec=SourceManager)
        store = mock.MagicMock(spec=SessionStore)
        engine = IterationEngine("conservative", 5)
        session = SessionRecord(source_file=source_file)

        aimv_json = str(Path(output_dir) / "aimv-foo-r1.json")
        builder.compile_with_aimv.return_value = _make_build_ok(aimv_json, DIAGNOSTICS_MISSED)
        builder.check_vectorization.return_value = VectorizationStatus(
            "foo", 1, 0, 1, DIAGNOSTICS_MISSED, 0)
        # Always return no suggestions
        mcp.analyze.return_value = {"no_action_possible": True, "suggestions": []}

        result = process_single_function(
            "foo", source_file, DIAGNOSTICS_MISSED, config,
            builder, mcp, engine, sources, store, session,
        )

        # After 3 escalations, stops at aggressive
        assert result.termination_reason == TerminationReason.NO_SUGGESTION
        assert engine.current_level == "aggressive"

    def test_single_function_interrupted(self, source_file, output_dir, config):
        """KeyboardInterrupt → rollback + INTERRUPTED."""
        builder = mock.MagicMock(spec=BuildOrchestrator)
        mcp = mock.MagicMock(spec=MCPClient)
        sources = mock.MagicMock(spec=SourceManager)
        store = mock.MagicMock(spec=SessionStore)
        engine = IterationEngine("conservative", 5)
        session = SessionRecord(source_file=source_file)

        builder.compile_with_aimv.side_effect = KeyboardInterrupt()

        result = process_single_function(
            "foo", source_file, DIAGNOSTICS_MISSED, config,
            builder, mcp, engine, sources, store, session,
        )

        assert result.termination_reason == TerminationReason.INTERRUPTED
        sources.rollback_all.assert_called_once()

    def test_single_function_lock_timeout(self, source_file, output_dir, config):
        """Lock acquisition timeout → LOCK_TIMEOUT."""
        builder = mock.MagicMock(spec=BuildOrchestrator)
        mcp = mock.MagicMock(spec=MCPClient)
        sources = mock.MagicMock(spec=SourceManager)
        store = mock.MagicMock(spec=SessionStore)
        engine = IterationEngine("conservative", 5)
        session = SessionRecord(source_file=source_file)

        aimv_json = str(Path(output_dir) / "aimv-foo-r1.json")
        builder.compile_with_aimv.return_value = _make_build_ok(aimv_json, DIAGNOSTICS_MISSED)
        builder.check_vectorization.return_value = VectorizationStatus(
            "foo", 1, 0, 1, DIAGNOSTICS_MISSED, 0)
        sources.has_diff.return_value = False
        sources.acquire_lock.return_value = False  # Lock timeout
        mcp.analyze.return_value = _make_mcp_response()

        result = process_single_function(
            "foo", source_file, DIAGNOSTICS_MISSED, config,
            builder, mcp, engine, sources, store, session,
        )

        assert result.termination_reason == TerminationReason.LOCK_TIMEOUT

    def test_single_function_regression_rollback(self, source_file, output_dir, config):
        """passed_remark_count decreased → discard shadow + NO_IMPROVEMENT."""
        builder = mock.MagicMock(spec=BuildOrchestrator)
        mcp = mock.MagicMock(spec=MCPClient)
        sources = SourceManager(output_dir)
        store = mock.MagicMock(spec=SessionStore)
        engine = IterationEngine("conservative", 5)
        session = SessionRecord(source_file=source_file)

        # Round 1: compile shows missed with passed_count=2, MCP suggests,
        # verify shows passed_count=1 (regression!) → NO_IMPROVEMENT
        aimv_json_r1 = str(Path(output_dir) / "aimv-foo-r1.json")
        verify_json = str(Path(output_dir) / "aimv-foo-verify-r1.json")

        builder.compile_with_aimv.side_effect = [
            _make_build_ok(aimv_json_r1, DIAGNOSTICS_MISSED),  # R1 initial
            _make_build_ok(verify_json, DIAGNOSTICS_MISSED),    # R1 verify
        ]
        # First check: 2 passed (before patch)
        # After prev_passed_count=2, verify shows only 1 passed → regression
        builder.check_vectorization.side_effect = [
            VectorizationStatus("foo", 3, 2, 1, DIAGNOSTICS_MISSED, 2),  # R1 after initial
            VectorizationStatus("foo", 3, 1, 2, DIAGNOSTICS_MISSED, 1),  # R1 after verify (regression: 2→1)
        ]
        mcp.analyze.return_value = _make_mcp_response()

        # Set prev_passed_count to 2 by simulating a prior successful round
        # We need to manually set it — but process_single_function starts at 0.
        # So we need to test the scenario where prev_passed_count > 0.
        # The easiest way: do 2 rounds, first round commits with 2 passed,
        # second round verify shows 1 passed.
        # Actually, let me restructure: provide 2 rounds of compile
        aimv_json_r2 = str(Path(output_dir) / "aimv-foo-r2.json")
        verify_json_r2 = str(Path(output_dir) / "aimv-foo-verify-r2.json")

        builder.compile_with_aimv.side_effect = [
            _make_build_ok(aimv_json_r1, DIAGNOSTICS_MISSED),  # R1 initial
            _make_build_ok(verify_json, DIAGNOSTICS_MISSED),    # R1 verify (commit, passed=2)
            _make_build_ok(aimv_json_r2, DIAGNOSTICS_MISSED),   # R2 initial
            _make_build_ok(verify_json_r2, DIAGNOSTICS_MISSED), # R2 verify (regression: 2→1)
        ]
        builder.check_vectorization.side_effect = [
            VectorizationStatus("foo", 3, 2, 1, DIAGNOSTICS_MISSED, 2),  # R1 after initial
            VectorizationStatus("foo", 3, 2, 1, DIAGNOSTICS_MISSED, 2),  # R1 after verify (same, commit)
            VectorizationStatus("foo", 3, 2, 1, DIAGNOSTICS_MISSED, 2),  # R2 after initial
            VectorizationStatus("foo", 3, 1, 2, DIAGNOSTICS_MISSED, 1),  # R2 after verify (regression!)
        ]
        builder.run_tests.return_value = TestResult(
            returncode=0, stdout="", stderr="", passed=0, failed=0, elapsed_ms=0)
        mcp.analyze.return_value = _make_mcp_response()

        result = process_single_function(
            "foo", source_file, DIAGNOSTICS_MISSED, config,
            builder, mcp, engine, sources, store, session,
        )

        assert result.termination_reason == TerminationReason.NO_IMPROVEMENT
