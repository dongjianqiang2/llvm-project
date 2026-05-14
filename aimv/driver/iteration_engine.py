# [BiSheng] AIMV Driver — Iteration strategy decision engine
from enum import Enum
from typing import Optional


class NextAction(Enum):
    CONTINUE = "continue"
    RETRY_SAME = "retry_same"
    ESCALATE_LEVEL = "escalate_level"
    ROLLBACK = "rollback"
    STOP = "stop"


class IterationEngine:
    def __init__(self, initial_level: str = "moderate", max_rounds: int = 5,
                 perf_degradation_threshold_pct: float = 5.0):
        self.initial_level = initial_level
        self.current_level = initial_level
        self.max_rounds = max_rounds
        self.degradation_threshold = perf_degradation_threshold_pct
        self._consecutive_compile_failures = 0
        self._consecutive_mcp_failures = 0

    def decide(
        self, current_round: int, build_result_ok: bool, test_result_ok: bool,
        vectorized: bool, mcp_had_suggestions: bool, mcp_responded: bool,
        perf_delta_pct: Optional[float] = None,
    ) -> tuple[NextAction, str]:
        if vectorized:
            return NextAction.STOP, "vectorization succeeded"
        if current_round >= self.max_rounds:
            return NextAction.STOP, f"reached max rounds ({self.max_rounds})"
        if not build_result_ok:
            self._consecutive_compile_failures += 1
            if self._consecutive_compile_failures >= 2:
                return NextAction.STOP, "consecutive compile failures"
            return NextAction.ROLLBACK, "compile failure, will retry"
        self._consecutive_compile_failures = 0
        # Test failure: always terminal (patch broke semantics)
        if not test_result_ok:
            return NextAction.ROLLBACK, "test failure, stopping"
        if not mcp_responded:
            self._consecutive_mcp_failures += 1
            if self._consecutive_mcp_failures >= 1:
                return NextAction.STOP, "MCP server unresponsive"
            return NextAction.RETRY_SAME, "MCP timeout, one retry"
        self._consecutive_mcp_failures = 0
        if not mcp_had_suggestions:
            if self._try_escalate():
                return NextAction.ESCALATE_LEVEL, f"escalated to {self.current_level}"
            return NextAction.STOP, "no suggestions at highest level"
        if perf_delta_pct is not None and perf_delta_pct < -self.degradation_threshold:
            return NextAction.ROLLBACK, f"performance degraded by {-perf_delta_pct:.1f}%"
        return NextAction.CONTINUE, "continuing to next round"

    def _try_escalate(self) -> bool:
        levels = ["conservative", "moderate", "aggressive"]
        idx = levels.index(self.current_level)
        if idx < len(levels) - 1:
            self.current_level = levels[idx + 1]
            return True
        return False

    def reset(self):
        self.current_level = self.initial_level
        self._consecutive_compile_failures = 0
        self._consecutive_mcp_failures = 0
