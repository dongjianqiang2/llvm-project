"""T2.3 — Prompt builder tests."""
import sys
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.mcp_server.models import (
    AnalyzeRequest, TargetInfo, FunctionInfo, SingleDiagnostic,
    RemarkSeverity, AimvLevel, CostModelDetail, DependencyInfo,
    MemoryInfo, LoopInfo,
)
from aimv.mcp_server.prompt_builder import build_system_prompt, build_user_prompt


VALID_TARGET = TargetInfo(
    triple="armv7-unknown-linux-gnueabi", cpu="cortex-a9",
    features=["neon", "vfp4"], vector_width=128)
VALID_FUNC = FunctionInfo(
    name="process_task",
    signature="void process_task(int *a, int *b, int n)",
    source_code="void process_task(int *a, int *b, int n) { for (int i=0;i<n;i++) a[i]=b[i]; }",
    source_file="task.c", loop_line=1)
VALID_DIAG = SingleDiagnostic(
    pass_name="LoopVectorize", remark_id="CantReorderMemOps",
    remark_text="unsafe dependent memory operations",
    severity=RemarkSeverity.MISSED, function_name="process_task",
    loop_location="task.c:1:1", source_context="for (int i=0;i<n;i++) a[i]=b[i];",
    ir_snippet="%gep_a = getelementptr ...",
    cost_model=CostModelDetail(scalar_cost=24, vector_cost=38, vf=4, interleave_count=1),
    dependencies=[DependencyInfo(
        dep_type="Backward", source_ptr="ptr %b", sink_ptr="ptr %a",
        alias_result="unsafe: prevents vectorization")],
    memory_info=MemoryInfo(
        num_stores=1, num_loads=2, stride="stride=1",
        max_alignment=4, memory_check_count=2, memory_check_cost=8),
    loop_info=LoopInfo(
        num_blocks=3, num_instructions=18, trip_count=-1,
        num_branches=1, num_calls=0))


@pytest.fixture
def request_full():
    return AnalyzeRequest(
        request_id="r1", target=VALID_TARGET,
        function=VALID_FUNC, diagnostics=[VALID_DIAG],
        aimv_level=AimvLevel.MODERATE)


class TestSystemPrompt:
    def test_contains_triple(self, request_full):
        prompt = build_system_prompt(request_full)
        assert "armv7" in prompt

    def test_contains_aimv_level(self, request_full):
        prompt = build_system_prompt(request_full)
        assert "moderate" in prompt

    def test_contains_rules(self, request_full):
        prompt = build_system_prompt(request_full)
        assert "DO NOT change program semantics" in prompt


class TestUserPrompt:
    def test_contains_function_signature(self, request_full):
        prompt = build_user_prompt(request_full)
        assert "process_task" in prompt
        assert "void process_task" in prompt

    def test_contains_dep_type(self, request_full):
        prompt = build_user_prompt(request_full)
        assert "Backward" in prompt

    def test_contains_cost_model(self, request_full):
        prompt = build_user_prompt(request_full)
        assert "24" in prompt
        assert "38" in prompt
        assert "VF=4" in prompt

    def test_empty_history_no_previous_attempts(self, request_full):
        prompt = build_user_prompt(request_full)
        assert "Previous Attempts" not in prompt

    def test_nonempty_history_shows_previous(self, request_full):
        req = AnalyzeRequest(
            request_id="r2", target=VALID_TARGET,
            function=VALID_FUNC, diagnostics=[VALID_DIAG],
            history=[{"round": 1, "suggestion_description": "add restrict",
                       "result": "failed"}])
        prompt = build_user_prompt(req)
        assert "Previous Attempts" in prompt
        assert "Do NOT repeat" in prompt

    def test_null_cost_model_shows_unavailable(self, request_full):
        diag_no_cost = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="UnsafeDep",
            remark_text="unsafe dependence",
            severity=RemarkSeverity.MISSED, function_name="foo",
            loop_location="f.c:1:1", source_context="", ir_snippet="",
            cost_model=None)
        req = AnalyzeRequest(
            request_id="r3", target=VALID_TARGET,
            function=VALID_FUNC, diagnostics=[diag_no_cost])
        prompt = build_user_prompt(req)
        assert "not available" in prompt.lower()
