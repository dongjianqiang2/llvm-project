"""T3.6 — IterationEngine decision matrix tests."""
import sys
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.iteration_engine import IterationEngine, NextAction


@pytest.fixture
def engine():
    return IterationEngine(initial_level="moderate", max_rounds=5)


class TestIterationEngine:
    def test_vectorized_stops(self, engine):
        action, reason = engine.decide(1, True, True, True, True, True)
        assert action == NextAction.STOP
        assert "vectorization succeeded" in reason

    def test_round_limit_stops(self, engine):
        action, reason = engine.decide(5, True, True, False, True, True)
        assert action == NextAction.STOP
        assert "max rounds" in reason

    def test_compile_failure_once_rollback(self, engine):
        action, reason = engine.decide(1, False, True, False, True, True)
        assert action == NextAction.ROLLBACK

    def test_compile_failure_twice_stops(self, engine):
        engine.decide(1, False, True, False, True, True)  # first
        action, reason = engine.decide(2, False, True, False, True, True)  # second
        assert action == NextAction.STOP
        assert "consecutive" in reason

    def test_test_failure_rollback(self, engine):
        action, reason = engine.decide(1, True, False, False, True, True)
        assert action == NextAction.ROLLBACK
        assert "test failure" in reason

    def test_mcp_no_response_stops(self, engine):
        action, reason = engine.decide(1, True, True, False, True, False)
        assert action == NextAction.STOP

    def test_mcp_no_suggestions_escalates(self, engine):
        action, reason = engine.decide(1, True, True, False, False, True)
        assert action == NextAction.ESCALATE_LEVEL
        assert engine.current_level == "aggressive"

    def test_mcp_no_suggestions_at_aggressive_stops(self, engine):
        engine.current_level = "aggressive"
        action, reason = engine.decide(1, True, True, False, False, True)
        assert action == NextAction.STOP

    def test_perf_degradation_rollback(self, engine):
        action, reason = engine.decide(1, True, True, False, True, True,
                                        perf_delta_pct=-10.0)
        assert action == NextAction.ROLLBACK

    def test_perf_small_degradation_continues(self, engine):
        action, reason = engine.decide(1, True, True, False, True, True,
                                        perf_delta_pct=-2.0)
        assert action == NextAction.CONTINUE

    def test_normal_continue(self, engine):
        action, reason = engine.decide(1, True, True, False, True, True)
        assert action == NextAction.CONTINUE

    def test_reset(self, engine):
        engine.current_level = "aggressive"
        engine.reset()
        assert engine.current_level == "moderate"
