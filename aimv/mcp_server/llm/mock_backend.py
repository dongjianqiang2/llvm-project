# [AIMV] MCP Server — Mock LLM Backend (offline testing, no real LLM needed)
import os
import re
from typing import Optional

from .base import AbstractLLMBackend
from ..models import (
    AnalyzeRequest, AnalyzeResponse, Suggestion, RemarkSeverity,
)


# Known diagnostic pattern → fixed suggestion mapping
PATTERN_SUGGESTIONS = {
    "CantReorderMemOps": {
        "description": "Add restrict qualifier to pointer parameters",
        "reasoning": "Alias analysis failed because pointers may alias. "
                     "Adding restrict tells the compiler they don't overlap.",
        "estimated_impact": "high",
        "safety_concern": "Restrict requires that the restricted pointers "
                          "do not alias in practice. Verify at runtime.",
    },
    "UnsafeDep": {
        "description": "Add restrict or restructure loop to eliminate dependency",
        "reasoning": "Unsafe memory dependency prevents vectorization. "
                     "Adding restrict or splitting the loop can help.",
        "estimated_impact": "medium",
        "safety_concern": "Ensure the dependency is truly spurious before adding restrict.",
    },
}


class MockLLMBackend(AbstractLLMBackend):
    """Mock LLM backend for offline testing.

    Returns fixed suggestions for known diagnostic patterns.
    Activated via AIMV_LLM_BACKEND=mock environment variable.

    Pattern mapping:
      - CantReorderMemOps → "add restrict to pointer parameters"
      - UnsafeDep → "add restrict or restructure loop"
      - Unknown → no_action_possible=True
    """

    def analyze(self, request: AnalyzeRequest) -> AnalyzeResponse:
        suggestions = []

        for diag in request.diagnostics:
            if diag.severity != RemarkSeverity.MISSED:
                continue

            remark_id = diag.remark_id
            if remark_id in PATTERN_SUGGESTIONS:
                pattern = PATTERN_SUGGESTIONS[remark_id]
                suggestion = self._build_suggestion(request, diag, pattern)
                suggestions.append(suggestion)

        if not suggestions:
            return AnalyzeResponse(
                request_id=request.request_id,
                suggestions=[],
                overall_analysis="No known pattern matches the provided diagnostics.",
                confidence=1.0,
                no_action_possible=True,
            )

        return AnalyzeResponse(
            request_id=request.request_id,
            suggestions=suggestions[:1],
            overall_analysis=f"Mock analysis: {suggestions[0].description}",
            confidence=0.85,
            no_action_possible=False,
        )

    def health_check(self) -> bool:
        return True

    def _build_suggestion(self, request: AnalyzeRequest, diag,
                          pattern: dict) -> Suggestion:
        """Build a Suggestion with all required fields."""
        source_file = request.function.source_file or "source.c"
        source_code = request.function.source_code
        lines = source_code.split("\n")

        loop_line = request.function.loop_line
        if loop_line < 1 or loop_line > len(lines):
            loop_line = 1

        # Find the function signature line (first line with the function name)
        func_line_idx = 0
        for i, line in enumerate(lines):
            if request.function.name in line:
                func_line_idx = i
                break

        func_line = lines[func_line_idx] if func_line_idx < len(lines) else ""
        line_start = func_line_idx + 1
        line_end = func_line_idx + 1

        # Try to add restrict to pointer parameters
        modified = self._add_restrict(func_line)
        if modified == func_line:
            # Fallback: add comment to target line
            target_line = lines[loop_line - 1] if loop_line <= len(lines) else ""
            modified = f"{target_line} /* AIMV: {pattern['description']} */"
            line_start = loop_line
            line_end = loop_line
            func_line = target_line

        diff = (
            f"--- a/{source_file}\n+++ b/{source_file}\n"
            f"@@ -{line_start},{line_end - line_start + 1} +{line_start},{line_end - line_start + 1} @@\n"
            f"-{func_line}\n+{modified}\n"
        )

        return Suggestion(
            description=pattern["description"],
            reasoning=pattern["reasoning"],
            source_file=source_file,
            line_start=line_start,
            line_end=line_end,
            original=func_line,
            modified=modified,
            diff=diff,
            estimated_impact=pattern.get("estimated_impact", "medium"),
            safety_concern=pattern.get("safety_concern"),
        )

    @staticmethod
    def _add_restrict(line: str) -> str:
        """Try to add restrict to first pointer parameter in a line."""
        # Match patterns like "int *a" or "float *b"
        modified = re.sub(
            r'(\w+\s*\*\s*)(\w+)',
            r'\1restrict \2',
            line,
            count=1,
        )
        return modified


def create_mock_backend() -> MockLLMBackend:
    """Factory function for MockLLMBackend."""
    return MockLLMBackend()
