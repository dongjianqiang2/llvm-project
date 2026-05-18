# [AIMV] T5.1-T5.3 — Stage 5: Diagnostic dimension expansion tests
import pytest

from aimv.mcp_server.models import (
    AnalyzeRequest, AnalyzeResponse, SingleDiagnostic, TargetInfo,
    FunctionInfo, RemarkSeverity, AimvLevel, CostModelDetail, MemoryInfo,
)
from aimv.mcp_server.llm.mock_backend import MockLLMBackend, PATTERN_SUGGESTIONS, ALIGN_SUGGESTION
from aimv.mcp_server.prompt_builder import build_user_prompt


VALID_TARGET = TargetInfo(
    triple="armv7-unknown-linux-gnueabi", cpu="cortex-a9",
    features=["neon"], vector_width=128)


# --- T5.1: Cost model reject ---


class TestCostRejectPrompt:
    """T5.1: Cost rejection diagnostic → prompt contains cost analysis."""

    def _make_cost_reject_request(self):
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="VectorizationNotBeneficial",
            remark_text="vectorization is not beneficial",
            severity=RemarkSeverity.MISSED, function_name="compute_sum",
            loop_location="cost.c:3:3", source_context="", ir_snippet="",
            cost_model=CostModelDetail(scalar_cost=5, vector_cost=12, vf=4, interleave_count=1),
        )
        return AnalyzeRequest(
            request_id="r-cost",
            target=VALID_TARGET,
            function=FunctionInfo(
                name="compute_sum",
                signature="int compute_sum(short *data, int n)",
                source_code="int compute_sum(short *data, int n) {\n  int sum = 0;\n  for (int i = 0; i < n; i++) {\n    if (data[i] > 0) sum += data[i];\n  }\n  return sum;\n}\n",
                source_file="cost.c", loop_line=3,
            ),
            diagnostics=[diag],
            aimv_level=AimvLevel.CONSERVATIVE,
        )

    def test_cost_reject_prompt_has_cost_analysis(self):
        req = self._make_cost_reject_request()
        prompt = build_user_prompt(req)
        assert "NOT profitable" in prompt

    def test_cost_reject_prompt_has_cost_ratio(self):
        req = self._make_cost_reject_request()
        prompt = build_user_prompt(req)
        assert "Cost ratio" in prompt

    def test_cost_reject_prompt_has_guidance_section(self):
        req = self._make_cost_reject_request()
        prompt = build_user_prompt(req)
        assert "Cost Model Rejection Guidance" in prompt
        assert "pragma" in prompt.lower()

    def test_cost_reject_remark_id_triggers_guidance(self):
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="VectorizationNotBeneficial",
            remark_text="not beneficial",
            severity=RemarkSeverity.MISSED, function_name="foo",
            loop_location="f.c:1:1", source_context="", ir_snippet="",
        )
        req = AnalyzeRequest(
            request_id="r1", target=VALID_TARGET,
            function=FunctionInfo(name="foo", signature="void foo()",
                                   source_code="void foo(){}", source_file="f.c", loop_line=1),
            diagnostics=[diag],
        )
        prompt = build_user_prompt(req)
        assert "Cost Model Rejection Guidance" in prompt


class TestCostRejectMockBackend:
    """T5.1: MockLLMBackend handles VectorizationNotBeneficial."""

    def test_cost_reject_returns_suggestion(self):
        backend = MockLLMBackend()
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="VectorizationNotBeneficial",
            remark_text="not beneficial",
            severity=RemarkSeverity.MISSED, function_name="compute_sum",
            loop_location="cost.c:3:3", source_context="", ir_snippet="",
        )
        req = AnalyzeRequest(
            request_id="r1", target=VALID_TARGET,
            function=FunctionInfo(
                name="compute_sum",
                signature="int compute_sum(short *data, int n)",
                source_code="int compute_sum(short *data, int n) {\n  int sum = 0;\n  for (int i = 0; i < n; i++) {\n    if (data[i] > 0) sum += data[i];\n  }\n  return sum;\n}\n",
                source_file="cost.c", loop_line=3,
            ),
            diagnostics=[diag],
        )
        resp = backend.analyze(req)
        assert resp.no_action_possible is False
        assert len(resp.suggestions) >= 1
        assert "pragma" in resp.suggestions[0].description.lower()

    def test_cost_reject_pragma_in_diff(self):
        backend = MockLLMBackend()
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="VectorizationNotBeneficial",
            remark_text="not beneficial",
            severity=RemarkSeverity.MISSED, function_name="compute_sum",
            loop_location="cost.c:3:3", source_context="", ir_snippet="",
        )
        req = AnalyzeRequest(
            request_id="r1", target=VALID_TARGET,
            function=FunctionInfo(
                name="compute_sum",
                signature="int compute_sum(short *data, int n)",
                source_code="int compute_sum(short *data, int n) {\n  int sum = 0;\n  for (int i = 0; i < n; i++) {\n    if (data[i] > 0) sum += data[i];\n  }\n  return sum;\n}\n",
                source_file="cost.c", loop_line=3,
            ),
            diagnostics=[diag],
        )
        resp = backend.analyze(req)
        diff = resp.suggestions[0].diff
        assert "pragma" in diff.lower()

    def test_cost_reject_pattern_in_suggestions_dict(self):
        assert "VectorizationNotBeneficial" in PATTERN_SUGGESTIONS
        assert "InterleavingNotBeneficial" in PATTERN_SUGGESTIONS


# --- T5.2: Memory/alignment ---


class TestAlignUnknownPrompt:
    """T5.2: Alignment unknown diagnostic → prompt contains alignment guidance."""

    def _make_align_request(self, max_alignment=0):
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="CantReorderMemOps",
            remark_text="unsafe dep",
            severity=RemarkSeverity.MISSED, function_name="copy_buffer",
            loop_location="align.c:3:3", source_context="", ir_snippet="",
            memory_info=MemoryInfo(
                num_stores=1, num_loads=1, num_pred_stores=None,
                max_alignment=max_alignment, stride="non-constant",
                memory_check_count=1, memory_check_cost=2),
        )
        return AnalyzeRequest(
            request_id="r-align",
            target=VALID_TARGET,
            function=FunctionInfo(
                name="copy_buffer",
                signature="void copy_buffer(char *src, char *dst, int n)",
                source_code="void copy_buffer(char *src, char *dst, int n) {\n  for (int i = 0; i < n; i++) {\n    dst[i] = src[i];\n  }\n}\n",
                source_file="align.c", loop_line=2,
            ),
            diagnostics=[diag],
            aimv_level=AimvLevel.CONSERVATIVE,
        )

    def test_align_zero_triggers_guidance(self):
        req = self._make_align_request(max_alignment=0)
        prompt = build_user_prompt(req)
        assert "Memory Alignment Guidance" in prompt
        assert "alignas" in prompt.lower() or "assume_aligned" in prompt.lower()

    def test_align_one_triggers_guidance(self):
        req = self._make_align_request(max_alignment=1)
        prompt = build_user_prompt(req)
        assert "Memory Alignment Guidance" in prompt

    def test_align_four_no_guidance(self):
        """max_alignment=4 is typical for int*, should not trigger alignment guidance."""
        req = self._make_align_request(max_alignment=4)
        prompt = build_user_prompt(req)
        assert "Memory Alignment Guidance" not in prompt

    def test_align_guidance_contains_stride(self):
        req = self._make_align_request(max_alignment=0)
        prompt = build_user_prompt(req)
        assert "stride" in prompt.lower()


class TestAlignUnknownMockBackend:
    """T5.2: MockLLMBackend handles alignment-unknown diagnostics."""

    def test_align_zero_returns_suggestion(self):
        backend = MockLLMBackend()
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="UnknownRemark",
            remark_text="unknown failure",
            severity=RemarkSeverity.MISSED, function_name="copy_buffer",
            loop_location="align.c:2:3", source_context="", ir_snippet="",
            memory_info=MemoryInfo(
                num_stores=1, num_loads=1, num_pred_stores=None,
                max_alignment=0, stride="non-constant",
                memory_check_count=1, memory_check_cost=2),
        )
        req = AnalyzeRequest(
            request_id="r1", target=VALID_TARGET,
            function=FunctionInfo(
                name="copy_buffer",
                signature="void copy_buffer(char *src, char *dst, int n)",
                source_code="void copy_buffer(char *src, char *dst, int n) {\n  for (int i = 0; i < n; i++) {\n    dst[i] = src[i];\n  }\n}\n",
                source_file="align.c", loop_line=2,
            ),
            diagnostics=[diag],
        )
        resp = backend.analyze(req)
        assert resp.no_action_possible is False
        assert len(resp.suggestions) >= 1
        assert "align" in resp.suggestions[0].description.lower()

    def test_align_suggestion_has_diff(self):
        backend = MockLLMBackend()
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="UnknownRemark",
            remark_text="failure",
            severity=RemarkSeverity.MISSED, function_name="copy_buffer",
            loop_location="align.c:2:3", source_context="", ir_snippet="",
            memory_info=MemoryInfo(
                num_stores=1, num_loads=1, num_pred_stores=None,
                max_alignment=0, stride="unit",
                memory_check_count=None, memory_check_cost=None),
        )
        req = AnalyzeRequest(
            request_id="r1", target=VALID_TARGET,
            function=FunctionInfo(
                name="copy_buffer",
                signature="void copy_buffer(char *src, char *dst, int n)",
                source_code="void copy_buffer(char *src, char *dst, int n) {\n  for (int i = 0; i < n; i++) {\n    dst[i] = src[i];\n  }\n}\n",
                source_file="align.c", loop_line=2,
            ),
            diagnostics=[diag],
        )
        resp = backend.analyze(req)
        assert resp.suggestions[0].diff != ""

    def test_align_suggestion_pattern_exists(self):
        assert "alignas" in ALIGN_SUGGESTION["description"].lower() or \
               "assume_aligned" in ALIGN_SUGGESTION["description"].lower()


# --- T5.3: Multi-dimension benchmark ---


class TestMultiDimensionBenchmark:
    """T5.3: multi_fail.c → mixed failures → multi-dimension support."""

    def test_mixed_diagnostics_get_suggestions(self):
        """Multiple diagnostics with different types → at least 1 suggestion."""
        backend = MockLLMBackend()
        diags = [
            SingleDiagnostic(
                pass_name="LoopVectorize", remark_id="CantReorderMemOps",
                remark_text="alias", severity=RemarkSeverity.MISSED,
                function_name="multi_loop", loop_location="multi.c:3:3",
                source_context="", ir_snippet="",
            ),
            SingleDiagnostic(
                pass_name="LoopVectorize", remark_id="VectorizationNotBeneficial",
                remark_text="not profitable", severity=RemarkSeverity.MISSED,
                function_name="multi_loop", loop_location="multi.c:7:3",
                source_context="", ir_snippet="",
                cost_model=CostModelDetail(scalar_cost=10, vector_cost=18, vf=4, interleave_count=1),
            ),
        ]
        req = AnalyzeRequest(
            request_id="r-multi",
            target=VALID_TARGET,
            function=FunctionInfo(
                name="multi_loop",
                signature="void multi_loop(int *a, int *b, short *c, int n)",
                source_code="void multi_loop(int *a, int *b, short *c, int n) {\n  for (int i = 0; i < n; i++) a[i] = b[i] + b[i+1];\n  for (int i = 0; i < n; i++) if (c[i] > 0) c[i] = c[i]*2;\n}\n",
                source_file="multi.c", loop_line=2,
            ),
            diagnostics=diags,
        )
        resp = backend.analyze(req)
        assert resp.no_action_possible is False
        assert len(resp.suggestions) >= 1

    def test_mixed_diagnostics_prompt_has_both_guidance(self):
        """Prompt for mixed diagnostics includes both cost and align guidance."""
        diags = [
            SingleDiagnostic(
                pass_name="LoopVectorize", remark_id="VectorizationNotBeneficial",
                remark_text="not profitable", severity=RemarkSeverity.MISSED,
                function_name="f", loop_location="f.c:1:1",
                source_context="", ir_snippet="",
                cost_model=CostModelDetail(scalar_cost=5, vector_cost=12, vf=4, interleave_count=1),
            ),
            SingleDiagnostic(
                pass_name="LoopVectorize", remark_id="CantReorderMemOps",
                remark_text="alias", severity=RemarkSeverity.MISSED,
                function_name="f", loop_location="f.c:2:1",
                source_context="", ir_snippet="",
                memory_info=MemoryInfo(
                    num_stores=1, num_loads=1, num_pred_stores=None,
                    max_alignment=0, stride="unit",
                    memory_check_count=1, memory_check_cost=2),
            ),
        ]
        req = AnalyzeRequest(
            request_id="r-mixed", target=VALID_TARGET,
            function=FunctionInfo(name="f", signature="void f()",
                                   source_code="void f(){}", source_file="f.c", loop_line=1),
            diagnostics=diags,
        )
        prompt = build_user_prompt(req)
        assert "Cost Model Rejection Guidance" in prompt
        assert "Memory Alignment Guidance" in prompt

    def test_benchmark_multi_fail_file_exists(self):
        import os
        bench_path = os.path.join(
            os.path.dirname(__file__), "..", "benchmarks", "multi_fail.c")
        assert os.path.isfile(bench_path)

    def test_benchmark_cost_reject_file_exists(self):
        import os
        bench_path = os.path.join(
            os.path.dirname(__file__), "..", "benchmarks", "cost_reject.c")
        assert os.path.isfile(bench_path)

    def test_benchmark_align_unknown_file_exists(self):
        import os
        bench_path = os.path.join(
            os.path.dirname(__file__), "..", "benchmarks", "align_unknown.c")
        assert os.path.isfile(bench_path)
