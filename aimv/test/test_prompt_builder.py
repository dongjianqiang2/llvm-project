# [AIMV] Tests for aimv/mcp_server/prompt_builder.py (T2.4)
import pytest
from aimv.mcp_server.models import (
    AnalyzeRequest, TargetInfo, FunctionInfo, SingleDiagnostic,
    RemarkSeverity, AimvLevel, CostModelDetail, DependencyInfo,
    MemoryInfo, LoopInfo, HistoryRecord,
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
        num_stores=1, num_loads=2, num_pred_stores=None,
        max_alignment=4, stride="stride=1",
        memory_check_count=None, memory_check_cost=None),
    loop_info=LoopInfo(
        num_blocks=3, num_instructions=18, trip_count=-1,
        num_branches=1, num_calls=0))


@pytest.fixture
def request_full():
    return AnalyzeRequest(
        request_id="r1", target=VALID_TARGET,
        function=VALID_FUNC, diagnostics=[VALID_DIAG],
        aimv_level=AimvLevel.CONSERVATIVE)


class TestSystemPrompt:
    def test_contains_triple(self, request_full):
        prompt = build_system_prompt(request_full)
        assert "armv7" in prompt

    def test_contains_aimv_level(self, request_full):
        prompt = build_system_prompt(request_full)
        assert "conservative" in prompt

    def test_contains_rules(self, request_full):
        prompt = build_system_prompt(request_full)
        assert "DO NOT change program semantics" in prompt
        assert "restrict" in prompt

    def test_contains_level_descriptions(self, request_full):
        prompt = build_system_prompt(request_full)
        assert "conservative: only add qualifiers" in prompt
        assert "moderate: also suggest loop fission" in prompt
        assert "aggressive: also suggest data structure changes" in prompt

    def test_prompt_level_escalation(self):
        req = AnalyzeRequest(
            request_id="r1", target=VALID_TARGET,
            function=VALID_FUNC, diagnostics=[VALID_DIAG],
            aimv_level=AimvLevel.AGGRESSIVE)
        prompt = build_system_prompt(req)
        assert "aggressive" in prompt
        assert "data structure changes" in prompt


class TestUserPrompt:
    def test_contains_function_signature(self, request_full):
        prompt = build_user_prompt(request_full)
        assert "process_task" in prompt
        assert "void process_task" in prompt

    def test_diagnostic_block_alias_case(self, request_full):
        prompt = build_user_prompt(request_full)
        assert "Backward" in prompt
        assert "Memory Dependencies" in prompt

    def test_diagnostic_block_vf_zero(self):
        """T2.4: VF=0 → 'not determined' not 'VF=0'."""
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="UnsafeDep",
            remark_text="unsafe dep", severity=RemarkSeverity.MISSED,
            function_name="foo", loop_location="f.c:1:1",
            source_context="", ir_snippet="",
            cost_model=CostModelDetail(scalar_cost=-1, vector_cost=-1, vf=0, interleave_count=0))
        req = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                             function=VALID_FUNC, diagnostics=[diag])
        prompt = build_user_prompt(req)
        assert "not determined" in prompt

    def test_diagnostic_block_source_accuracy(self):
        """T2.4: source_accuracy='approximate' → WARNING."""
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="CantReorderMemOps",
            remark_text="loop not vectorized", severity=RemarkSeverity.MISSED,
            function_name="foo", loop_location="f.c:1:1",
            source_context="", ir_snippet="",
            source_accuracy="approximate")
        req = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                             function=VALID_FUNC, diagnostics=[diag])
        prompt = build_user_prompt(req)
        assert "WARNING" in prompt
        assert "approximate" in prompt

    def test_contains_cost_model(self, request_full):
        prompt = build_user_prompt(request_full)
        assert "24" in prompt
        assert "38" in prompt
        assert "VF=4" in prompt

    def test_cost_model_not_profitable_insight(self, request_full):
        """Cost model: vector_cost > scalar_cost → NOT profitable insight."""
        prompt = build_user_prompt(request_full)
        assert "NOT profitable" in prompt

    def test_empty_history_no_previous_attempts(self, request_full):
        prompt = build_user_prompt(request_full)
        assert "Previous Attempts" not in prompt

    def test_history_block_not_repeat(self):
        """T2.4: History with 'Do NOT repeat' indicator."""
        h = HistoryRecord(round=1, diagnosis_summary="CantReorderMemOps",
                          suggestion_applied="added restrict", outcome="still_failed")
        req = AnalyzeRequest(
            request_id="r1", target=VALID_TARGET,
            function=VALID_FUNC, diagnostics=[VALID_DIAG],
            history=[h])
        prompt = build_user_prompt(req)
        assert "Previous Attempts" in prompt
        assert "Do NOT repeat" in prompt

    def test_null_cost_model_shows_unavailable(self):
        diag_no_cost = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="UnsafeDep",
            remark_text="unsafe dependence",
            severity=RemarkSeverity.MISSED, function_name="foo",
            loop_location="f.c:1:1", source_context="", ir_snippet="",
            cost_model=None)
        req = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                             function=VALID_FUNC, diagnostics=[diag_no_cost])
        prompt = build_user_prompt(req)
        assert "not available" in prompt.lower()

    def test_dep_type_semantics_in_prompt(self, request_full):
        """T2.4: dep_type semantics explanation is included."""
        prompt = build_user_prompt(request_full)
        assert "DepType semantics" in prompt

    def test_memory_info_with_none_fields(self, request_full):
        """T2.4: MemoryInfo with Optional fields as None → N/A in prompt."""
        prompt = build_user_prompt(request_full)
        assert "N/A" in prompt

    def test_loop_info_trip_count_unknown(self, request_full):
        """T2.4: trip_count=-1 → 'unknown' in prompt."""
        prompt = build_user_prompt(request_full)
        assert "unknown" in prompt
