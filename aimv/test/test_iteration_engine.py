# [AIMV] T3.4 — IterationEngine decision matrix tests
import pytest

from aimv.driver.iteration_engine import IterationEngine
from aimv.driver.models import NextAction


@pytest.fixture
def engine():
    return IterationEngine(initial_level="conservative", max_rounds=5)


class TestDecideVectorized:
    def test_decide_vectorized_success(self, engine):
        """vectorized=True → (STOP, "vectorization succeeded")."""
        action, reason = engine.decide(1, True, True, True, True, True)
        assert action == NextAction.STOP
        assert "vectorization succeeded" in reason


class TestDecideRoundLimit:
    def test_decide_round_limit(self, engine):
        """current_round >= max_rounds → (STOP, ...)."""
        action, reason = engine.decide(5, True, True, False, True, True)
        assert action == NextAction.STOP
        assert "max rounds" in reason


class TestDecideCompileFailures:
    def test_decide_patch_compile_fail_rollback(self, engine):
        """compile_phase="patch", 1st failure → (ROLLBACK, ...)."""
        action, reason = engine.decide(
            1, False, True, False, True, True, compile_phase="patch")
        assert action == NextAction.ROLLBACK
        assert "patch compile failure" in reason

    def test_decide_patch_compile_fail_stop(self, engine):
        """compile_phase="patch", 2nd consecutive → (STOP, ...)."""
        engine.decide(1, False, True, False, True, True, compile_phase="patch")
        action, reason = engine.decide(
            2, False, True, False, True, True, compile_phase="patch")
        assert action == NextAction.STOP
        assert "consecutive patch compile failures" in reason

    def test_decide_source_compile_fail_stop(self, engine):
        """compile_phase="source" → directly STOP."""
        action, reason = engine.decide(
            1, False, True, False, True, True, compile_phase="source")
        assert action == NextAction.STOP
        assert "source compile failure" in reason

    def test_counters_independent(self, engine):
        """Patch compile failure does not affect source counter."""
        engine.decide(1, False, True, False, True, True, compile_phase="patch")
        assert engine._consecutive_compile_failures == 1
        assert engine._consecutive_source_compile_failures == 0

        # Source compile failure
        engine.decide(2, False, True, False, True, True, compile_phase="source")
        assert engine._consecutive_source_compile_failures == 1

        # Successful build resets patch counter
        engine._consecutive_compile_failures = 0
        engine.decide(3, True, True, False, True, True, compile_phase="patch")
        assert engine._consecutive_compile_failures == 0
        assert engine._consecutive_source_compile_failures == 0


class TestDecideTestFailure:
    def test_decide_test_failure_rollback(self, engine):
        """test_result_ok=False → (ROLLBACK, ...)."""
        action, reason = engine.decide(1, True, False, False, True, True)
        assert action == NextAction.ROLLBACK
        assert "test failure" in reason


class TestDecideEscalation:
    def test_decide_no_suggestion_escalate(self, engine):
        """No suggestion → (ESCALATE_LEVEL, ...), level conservative → moderate."""
        action, reason = engine.decide(1, True, True, False, False, True)
        assert action == NextAction.ESCALATE_LEVEL
        assert engine.current_level == "moderate"

    def test_decide_escalate_to_aggressive_enables_review(self, engine):
        """Level escalation to aggressive → _review_mode = True."""
        engine.current_level = "moderate"
        action, reason = engine.decide(1, True, True, False, False, True)
        assert action == NextAction.ESCALATE_LEVEL
        assert engine.current_level == "aggressive"
        assert engine._review_mode is True

    def test_decide_no_suggestion_at_aggressive_stops(self, engine):
        """No suggestion at aggressive → (STOP, ...)."""
        engine.current_level = "aggressive"
        action, reason = engine.decide(1, True, True, False, False, True)
        assert action == NextAction.STOP


class TestDecideRegression:
    def test_decide_regression_rollback(self, engine):
        """passed_remark_delta < 0 → (ROLLBACK, ...)."""
        action, reason = engine.decide(
            1, True, True, False, True, True, passed_remark_delta=-1)
        assert action == NextAction.ROLLBACK
        assert "regression" in reason

    def test_decide_no_regression(self, engine):
        """passed_remark_delta >= 0 → CONTINUE."""
        action, reason = engine.decide(
            1, True, True, False, True, True, passed_remark_delta=1)
        assert action == NextAction.CONTINUE


class TestDecideMCPFailures:
    def test_decide_mcp_unresponsive(self, engine):
        """MCP not responded → STOP."""
        action, reason = engine.decide(1, True, True, False, True, False)
        assert action == NextAction.STOP
        assert "MCP" in reason

    def test_decide_perf_degradation_rollback(self, engine):
        """Perf degradation > threshold → ROLLBACK."""
        action, reason = engine.decide(
            1, True, True, False, True, True, perf_delta_pct=-10.0)
        assert action == NextAction.ROLLBACK

    def test_decide_perf_small_degradation_continues(self, engine):
        """Perf degradation within threshold → CONTINUE."""
        action, reason = engine.decide(
            1, True, True, False, True, True, perf_delta_pct=-2.0)
        assert action == NextAction.CONTINUE

    def test_decide_normal_continue(self, engine):
        """Normal case → CONTINUE."""
        action, reason = engine.decide(1, True, True, False, True, True)
        assert action == NextAction.CONTINUE


class TestReset:
    def test_reset(self, engine):
        engine.current_level = "aggressive"
        engine._review_mode = True
        engine._consecutive_compile_failures = 2
        engine._consecutive_source_compile_failures = 1
        engine.reset()
        assert engine.current_level == "conservative"
        assert engine._review_mode is False
        assert engine._consecutive_compile_failures == 0
        assert engine._consecutive_source_compile_failures == 0
