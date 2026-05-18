# [AIMV] Tests for aimv/mcp_server/models.py (T2.1)
import pytest
from aimv.mcp_server.models import (
    AnalyzeRequest, AnalyzeResponse, SingleDiagnostic,
    DependencyInfo, CostModelDetail, MemoryInfo, LoopInfo,
    TargetInfo, FunctionInfo, Suggestion, HistoryRecord,
    RemarkSeverity, AimvLevel,
)

VALID_TARGET = TargetInfo(
    triple="armv7-unknown-linux-gnueabi", cpu="cortex-a9",
    features=["neon"], vector_width=128)
VALID_FUNC = FunctionInfo(
    name="foo", signature="void foo(int *a, int n)",
    source_code="void foo(int *a, int n) { for(int i=0;i<n;i++) a[i]=0; }",
    source_file="test.c", loop_line=2)
VALID_DIAG = SingleDiagnostic(
    pass_name="LoopVectorize", remark_id="CantReorderMemOps",
    remark_text="loop not vectorized", severity=RemarkSeverity.MISSED,
    function_name="foo", loop_location="test.c:2:5",
    source_context="for(int i=0;i<n;i++) a[i]=0;", ir_snippet="...")


class TestAnalyzeRequestValidation:
    def test_valid_request_passes(self):
        req = AnalyzeRequest(
            request_id="test-1", target=VALID_TARGET, function=VALID_FUNC,
            diagnostics=[VALID_DIAG])
        assert req.request_id == "test-1"

    def test_empty_diagnostics_raises(self):
        with pytest.raises(Exception):
            AnalyzeRequest(request_id="t", target=VALID_TARGET,
                           function=VALID_FUNC, diagnostics=[])

    def test_too_many_diagnostics_raises(self):
        diags = [VALID_DIAG] * 21
        with pytest.raises(Exception):
            AnalyzeRequest(request_id="t", target=VALID_TARGET,
                           function=VALID_FUNC, diagnostics=diags)

    def test_model_validate_from_json(self):
        data = {
            "request_id": "r1",
            "target": {"triple": "armv7-unknown-linux-gnueabi", "cpu": "cortex-a9",
                        "features": ["neon"], "vector_width": 128},
            "function": {"name": "foo", "signature": "void f()",
                          "source_code": "void f(){}", "source_file": "f.c",
                          "loop_line": 1},
            "diagnostics": [{
                "pass_name": "LoopVectorize", "remark_id": "CantReorderMemOps",
                "remark_text": "failed", "severity": "missed",
                "function_name": "foo", "loop_location": "f.c:1:1",
                "source_context": "", "ir_snippet": "",
            }],
        }
        req = AnalyzeRequest.model_validate(data)
        assert req.request_id == "r1"


class TestMemoryInfoOptionalFields:
    """T2.1: num_pred_stores, memory_check_count, memory_check_cost are Optional[int]=None."""

    def test_memory_info_pred_stores_none(self):
        mi = MemoryInfo(
            num_stores=1, num_loads=2, num_pred_stores=None,
            max_alignment=4, stride="stride=1",
            memory_check_count=None, memory_check_cost=None,
        )
        assert mi.num_pred_stores is None
        assert mi.memory_check_count is None
        assert mi.memory_check_cost is None

    def test_memory_info_pred_stores_zero(self):
        mi = MemoryInfo(
            num_stores=1, num_loads=2, num_pred_stores=0,
            max_alignment=4, stride="stride=1",
            memory_check_count=2, memory_check_cost=8,
        )
        assert mi.num_pred_stores == 0
        assert mi.memory_check_count == 2
        assert mi.memory_check_cost == 8


class TestDependencyInfoDepType:
    """T2.1: dep_type must match LLVM 8 DepType values."""

    @pytest.mark.parametrize("dep_type", [
        "Backward", "Forward", "IndirectUnsafe", "Unknown", "NoDep",
        "ForwardButPreventsForwarding",
        "BackwardVectorizable",
        "BackwardVectorizableButPreventsForwarding",
    ])
    def test_valid_dep_types_accepted(self, dep_type):
        dep = DependencyInfo(dep_type=dep_type, source_ptr="a",
                             sink_ptr="b", alias_result="safe")
        assert dep.dep_type == dep_type

    @pytest.mark.parametrize("dep_type", ["RAW", "WAR", "WAW", "MayAlias"])
    def test_old_dep_types_rejected(self, dep_type):
        with pytest.raises(Exception):
            DependencyInfo(dep_type=dep_type, source_ptr="a", sink_ptr="b",
                           alias_result="safe")


class TestLoopInfoTripCount:
    """T2.1: trip_count semantics: -1=unavailable, 0=empty, >0=specific."""

    @pytest.mark.parametrize("tc", [-1, 0, 42])
    def test_loop_info_trip_count_semantics(self, tc):
        li = LoopInfo(num_blocks=3, num_instructions=18, trip_count=tc,
                       num_branches=1, num_calls=0)
        assert li.trip_count == tc


class TestSingleDiagnosticSourceAccuracy:
    """T2.1: source_accuracy field."""

    def test_source_accuracy_none(self):
        d = SingleDiagnostic(
            pass_name="LV", remark_id="test", remark_text="t",
            severity=RemarkSeverity.MISSED, function_name="f",
            loop_location="f.c:1:1", source_context="", ir_snippet="",
        )
        assert d.source_accuracy is None

    def test_source_accuracy_approximate(self):
        d = SingleDiagnostic(
            pass_name="LV", remark_id="test", remark_text="t",
            severity=RemarkSeverity.MISSED, function_name="f",
            loop_location="f.c:1:1", source_context="", ir_snippet="",
            source_accuracy="approximate",
        )
        assert d.source_accuracy == "approximate"


class TestAnalyzeResponseNoAction:
    """T2.1: no_action_possible=True with empty suggestions is valid."""

    def test_no_action_possible_response(self):
        resp = AnalyzeResponse(
            request_id="r1", suggestions=[],
            overall_analysis="nothing can be done", confidence=1.0,
            no_action_possible=True)
        assert resp.no_action_possible is True

    def test_confidence_range(self):
        with pytest.raises(Exception):
            AnalyzeResponse(request_id="r1", suggestions=[],
                            overall_analysis="", confidence=1.5)


class TestSuggestionEstimatedImpact:
    def test_valid_impact_values(self):
        for v in ["high", "medium", "low"]:
            s = Suggestion(description="d", reasoning="r",
                           source_file="f.c", line_start=1, line_end=1,
                           original="x", modified="y", diff="---",
                           estimated_impact=v)
            assert s.estimated_impact == v

    def test_invalid_impact_rejected(self):
        with pytest.raises(Exception):
            Suggestion(description="d", reasoning="r",
                       source_file="f.c", line_start=1, line_end=1,
                       original="x", modified="y", diff="---",
                       estimated_impact="critical")


class TestHistoryRecord:
    """T2.1: HistoryRecord with structured fields."""

    def test_history_record_fields(self):
        h = HistoryRecord(
            round=1,
            diagnosis_summary="CantReorderMemOps: unsafe deps",
            suggestion_applied="added restrict to parameter 'a'",
            outcome="compile_passed, vectorization_still_failed",
        )
        assert h.round == 1
        assert h.suggestion_applied == "added restrict to parameter 'a'"

    def test_history_in_request(self):
        h = HistoryRecord(round=1, diagnosis_summary="d", suggestion_applied="s", outcome="o")
        req = AnalyzeRequest(
            request_id="t", target=VALID_TARGET, function=VALID_FUNC,
            diagnostics=[VALID_DIAG], history=[h],
        )
        assert len(req.history) == 1
        assert req.history[0].round == 1


class TestAimvLevelDefault:
    """T2.1: default aimv_level is CONSERVATIVE."""

    def test_default_level(self):
        req = AnalyzeRequest(
            request_id="t", target=VALID_TARGET, function=VALID_FUNC,
            diagnostics=[VALID_DIAG],
        )
        assert req.aimv_level == AimvLevel.CONSERVATIVE
