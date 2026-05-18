# [AIMV] AIMV Driver — Iteration strategy decision engine
from typing import Optional

from .models import NextAction


class IterationEngine:
    def __init__(self, initial_level: str = "conservative", max_rounds: int = 5,
                 perf_degradation_threshold_pct: float = 5.0):
        self.initial_level = initial_level
        self.current_level = initial_level
        self.max_rounds = max_rounds
        self.degradation_threshold = perf_degradation_threshold_pct
        self._consecutive_compile_failures = 0          # patch compile
        self._consecutive_source_compile_failures = 0   # source compile
        self._consecutive_mcp_failures = 0
        self._review_mode = False

    def decide(
        self, current_round: int, build_result_ok: bool, test_result_ok: bool,
        vectorized: bool, mcp_had_suggestions: bool, mcp_responded: bool,
        compile_phase: str = "patch",
        passed_remark_delta: Optional[int] = None,
        perf_delta_pct: Optional[float] = None,
    ) -> tuple[NextAction, str]:
        # ── Terminal: vectorization succeeded ──
        if vectorized:
            return NextAction.STOP, "vectorization succeeded"

        # ── Terminal: max rounds ──
        if current_round >= self.max_rounds:
            return NextAction.STOP, f"reached max rounds ({self.max_rounds})"

        # ── Source compile failure: always terminal ──
        if not build_result_ok and compile_phase == "source":
            self._consecutive_source_compile_failures += 1
            return NextAction.STOP, "source compile failure (unrecoverable)"

        # ── Patch compile failure: rollback, then stop on 2nd consecutive ──
        if not build_result_ok:
            self._consecutive_compile_failures += 1
            if self._consecutive_compile_failures >= 2:
                return NextAction.STOP, "consecutive patch compile failures"
            return NextAction.ROLLBACK, "patch compile failure, will rollback and retry"
        self._consecutive_compile_failures = 0
        self._consecutive_source_compile_failures = 0

        # ── Test failure: rollback + stop ──
        if not test_result_ok:
            return NextAction.ROLLBACK, "test failure, stopping"

        # ── Regression: passed remark count decreased ──
        if passed_remark_delta is not None and passed_remark_delta < 0:
            return NextAction.ROLLBACK, f"vectorization regression ({passed_remark_delta} passed remarks lost)"

        # ── MCP unresponsive ──
        if not mcp_responded:
            self._consecutive_mcp_failures += 1
            if self._consecutive_mcp_failures >= 1:
                return NextAction.STOP, "MCP server unresponsive"
            return NextAction.RETRY_SAME, "MCP timeout, one retry"
        self._consecutive_mcp_failures = 0

        # ── No suggestions: try escalate ──
        if not mcp_had_suggestions:
            if self._try_escalate():
                return NextAction.ESCALATE_LEVEL, f"escalated to {self.current_level}"
            return NextAction.STOP, "no suggestions at highest level"

        # ── Performance degradation ──
        if perf_delta_pct is not None and perf_delta_pct < -self.degradation_threshold:
            return NextAction.ROLLBACK, f"performance degraded by {-perf_delta_pct:.1f}%"

        return NextAction.CONTINUE, "continuing to next round"

    def _try_escalate(self) -> bool:
        levels = ["conservative", "moderate", "aggressive"]
        idx = levels.index(self.current_level)
        if idx < len(levels) - 1:
            self.current_level = levels[idx + 1]
            # Auto-enable review mode when escalating to aggressive
            if self.current_level == "aggressive":
                self._review_mode = True
            return True
        return False

    def reset(self):
        self.current_level = self.initial_level
        self._consecutive_compile_failures = 0
        self._consecutive_source_compile_failures = 0
        self._consecutive_mcp_failures = 0
        self._review_mode = False
