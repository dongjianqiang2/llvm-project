# [AIMV] MCP Server — Mock LLM Backend (offline testing, no real LLM needed)
import re

from .base import AbstractLLMBackend
from ..models import (
    AnalyzeRequest, AnalyzeResponse, Suggestion, RemarkSeverity,
)


# Known diagnostic pattern → fixed suggestion mapping
PATTERN_SUGGESTIONS = {
    # LoopVectorize patterns
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
    "VectorizationNotBeneficial": {
        "description": "Add #pragma clang loop vectorize(enable) to override cost model",
        "reasoning": "The cost model estimates vectorization is not profitable. "
                     "A pragma override forces the compiler to vectorize, which may "
                     "be beneficial when trip counts are larger than the model assumes.",
        "estimated_impact": "medium",
        "safety_concern": "Forcing vectorization when the cost model rejects it "
                          "may degrade performance for small trip counts.",
    },
    "InterleavingNotBeneficial": {
        "description": "Add #pragma clang loop vectorize(enable) to override cost model",
        "reasoning": "Interleaving was rejected by the cost model. "
                     "A pragma override can enable vectorization.",
        "estimated_impact": "low",
        "safety_concern": "Same as VectorizationNotBeneficial — may hurt small trip counts.",
    },
    "InterleavingNotBeneficialAndDisabled": {
        "description": "Add #pragma clang loop vectorize(enable) to override cost model",
        "reasoning": "Interleaving was rejected and disabled. "
                     "A pragma override can enable vectorization with interleaving.",
        "estimated_impact": "low",
        "safety_concern": "Forcing vectorization may degrade performance for small trip counts.",
    },
    # SLP Vectorizer patterns
    "NotBeneficial": {
        "description": "Reorder scalar operations to enable SLP vectorization",
        "reasoning": "SLP vectorization was possible but not beneficial due to "
                     "pack/unpack overhead. Reordering operations or using wider "
                     "types can reduce overhead.",
        "estimated_impact": "medium",
        "safety_concern": "Operation reordering must preserve program semantics.",
    },
    "NotPossible": {
        "description": "Restructure scalar code to enable SLP vectorization",
        "reasoning": "SLP could not find a vectorizable pattern. Consider "
                     "combining adjacent scalar operations or using explicit "
                     "SIMD intrinsics.",
        "estimated_impact": "low",
        "safety_concern": "Manual restructuring may reduce code readability.",
    },
    "UnsupportedType": {
        "description": "Use standard SIMD-friendly types for SLP vectorization",
        "reasoning": "The operation uses a type not supported by SIMD. "
                     "Convert to standard integer or float types.",
        "estimated_impact": "medium",
        "safety_concern": "Type changes may affect precision or overflow behavior.",
    },
    "SmallVF": {
        "description": "Merge adjacent computations to increase SLP vector width",
        "reasoning": "Too few instructions to form a profitable vector. "
                     "Unroll the loop or merge adjacent computations.",
        "estimated_impact": "low",
        "safety_concern": "Merging computations may increase register pressure.",
    },
    # LoopUnroll patterns
    "CantUnrollTripCount": {
        "description": "Add __builtin_assume(n >= 16) to hint trip count range",
        "reasoning": "Trip count is unknown at compile time, preventing unroll "
                     "decisions. Adding assume hints enables the compiler to "
                     "choose a profitable unroll factor.",
        "estimated_impact": "high",
        "safety_concern": "Assumed bounds must hold at runtime. Wrong hints "
                          "cause undefined behavior.",
    },
    "UnrollNotBeneficial": {
        "description": "Use loop fission to split large loop body for unrolling",
        "reasoning": "The unrolled loop body exceeds size threshold. "
                     "Splitting the loop into smaller parts allows selective unrolling.",
        "estimated_impact": "medium",
        "safety_concern": "Loop fission may change memory access patterns.",
    },
    "UnrollTooExpensive": {
        "description": "Add #pragma clang loop unroll(enable) with explicit count",
        "reasoning": "Runtime unrolling is disabled by default due to cost. "
                     "A pragma with explicit count enables controlled unrolling.",
        "estimated_impact": "medium",
        "safety_concern": "Explicit unroll factors increase code size.",
    },
}

# Alignment-based pattern: triggered when max_alignment <= 1
ALIGN_SUGGESTION = {
    "description": "Add alignas(16) to pointer targets or use __builtin_assume_aligned",
    "reasoning": "Unknown or zero alignment prevents the compiler from using "
                 "aligned vector load/store instructions. Adding alignment hints "
                 "enables more efficient vector code generation.",
    "estimated_impact": "high",
    "safety_concern": "alignas and __builtin_assume_aligned require the memory "
                      "to actually be aligned at runtime. Misaligned access is undefined behavior.",
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
            # Accept both missed (LV) and analysis (SLP/Unroll) severities
            if diag.severity not in (RemarkSeverity.MISSED, RemarkSeverity.ANALYSIS):
                continue

            remark_id = diag.remark_id
            if remark_id in PATTERN_SUGGESTIONS:
                pattern = PATTERN_SUGGESTIONS[remark_id]
                suggestion = self._build_suggestion(request, diag, pattern)
                suggestions.append(suggestion)
            elif diag.memory_info and diag.memory_info.max_alignment <= 1:
                suggestion = self._build_suggestion(request, diag, ALIGN_SUGGESTION)
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

        # Find the for-loop line (contains 'for')
        loop_line_idx = loop_line - 1
        for i, line in enumerate(lines):
            stripped = line.strip()
            if stripped.startswith("for "):
                loop_line_idx = i
                break

        # Determine modification strategy based on pattern
        desc = pattern["description"]
        if "pragma" in desc.lower():
            modified, line_start, line_end, original = self._add_pragma(lines, loop_line_idx)
        elif "alignas" in desc.lower() or "assume_aligned" in desc.lower():
            modified, line_start, line_end, original = self._add_alignas(lines, loop_line_idx)
        elif "restrict" in desc.lower():
            modified, line_start, line_end, original = self._add_restrict_to_func(lines, request.function.name)
        else:
            # Generic fallback: add comment
            target_line = lines[loop_line_idx]
            modified = f"{target_line} /* AIMV: {desc} */"
            line_start = loop_line_idx + 1
            line_end = loop_line_idx + 1
            original = target_line

        diff = (
            f"--- a/{source_file}\n+++ b/{source_file}\n"
            f"@@ -{line_start},{line_end - line_start + 1} +{line_start},{line_end - line_start + 1} @@\n"
            f"-{original}\n+{modified}\n"
        )

        return Suggestion(
            description=pattern["description"],
            reasoning=pattern["reasoning"],
            source_file=source_file,
            line_start=line_start,
            line_end=line_end,
            original=original,
            modified=modified,
            diff=diff,
            estimated_impact=pattern.get("estimated_impact", "medium"),
            safety_concern=pattern.get("safety_concern"),
        )

    @staticmethod
    def _add_pragma(lines, loop_line_idx):
        """Add #pragma clang loop vectorize(enable) before the for-loop."""
        indent = len(lines[loop_line_idx]) - len(lines[loop_line_idx].lstrip())
        pragma_line = " " * indent + "#pragma clang loop vectorize(enable)"
        modified = pragma_line + "\n" + lines[loop_line_idx]
        line_start = loop_line_idx + 1
        line_end = loop_line_idx + 1
        original = lines[loop_line_idx]
        return modified, line_start, line_end, original

    @staticmethod
    def _add_alignas(lines, loop_line_idx):
        """Add __builtin_assume_aligned before the for-loop."""
        # Find pointer declarations before the loop
        for i in range(loop_line_idx - 1, -1, -1):
            if "*" in lines[i] and ("char" in lines[i] or "void" in lines[i] or "int" in lines[i]):
                # Add assume_aligned after the declaration line
                modified = lines[i] + "\n  src = __builtin_assume_aligned(src, 16);"
                line_start = i + 1
                line_end = i + 1
                return modified, line_start, line_end, lines[i]
        # Fallback: add pragma before loop
        indent = len(lines[loop_line_idx]) - len(lines[loop_line_idx].lstrip())
        hint_line = " " * indent + "/* AIMV: add alignas(16) to buffer declarations */"
        modified = hint_line + "\n" + lines[loop_line_idx]
        line_start = loop_line_idx + 1
        line_end = loop_line_idx + 1
        return modified, line_start, line_end, lines[loop_line_idx]

    @staticmethod
    def _add_restrict_to_func(lines, func_name):
        """Add restrict to first pointer parameter in function signature."""
        func_line_idx = 0
        for i, line in enumerate(lines):
            if func_name in line:
                func_line_idx = i
                break

        func_line = lines[func_line_idx] if func_line_idx < len(lines) else ""
        modified = re.sub(
            r'(\w+\s*\*\s*)(\w+)',
            r'\1restrict \2',
            func_line,
            count=1,
        )
        line_start = func_line_idx + 1
        line_end = func_line_idx + 1
        if modified == func_line:
            # No pointer parameter found; return unchanged
            pass
        return modified, line_start, line_end, func_line


def create_mock_backend() -> MockLLMBackend:
    """Factory function for MockLLMBackend."""
    return MockLLMBackend()
