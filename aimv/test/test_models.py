"""T0.6 — MCP Pydantic model validation tests."""
import sys
import json
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.mcp_server.models import (
    AnalyzeRequest, AnalyzeResponse, SingleDiagnostic,
    DependencyInfo, CostModelDetail, MemoryInfo, LoopInfo,
    TargetInfo, FunctionInfo, Suggestion,
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

    @pytest.mark.parametrize("dep_type", [
        "Backward", "Forward", "IndirectUnsafe", "Unknown", "NoDep",
        "ForwardButPreventsForwarding",
        "BackwardVectorizable",
        "BackwardVectorizableButPreventsForwarding",
    ])
    def test_valid_dep_types_accepted(self, dep_type):
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="CantReorderMemOps",
            remark_text="loop not vectorized", severity=RemarkSeverity.MISSED,
            function_name="foo", loop_location="t.c:1:1",
            source_context="", ir_snippet="",
            dependencies=[
                DependencyInfo(dep_type=dep_type, source_ptr="a",
                               sink_ptr="b", alias_result="safe")
            ])
        req = AnalyzeRequest(request_id="t", target=VALID_TARGET,
                             function=VALID_FUNC, diagnostics=[diag])
        assert req is not None

    @pytest.mark.parametrize("dep_type", ["RAW", "WAR", "WAW", "MayAlias", "UnknownAlias"])
    def test_old_dep_types_rejected(self, dep_type):
        match = None
        try:
            DependencyInfo(dep_type=dep_type, source_ptr="a", sink_ptr="b",
                           alias_result="safe")
        except Exception as e:
            match = e
        assert match is not None, f"dep_type={dep_type} should be rejected"

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


class TestAnalyzeResponseValidation:
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

    def test_estimated_impact_validation(self):
        valid = ["high", "medium", "low"]
        for v in valid:
            s = Suggestion(description="d", reasoning="r",
                           source_file="f.c", line_start=1, line_end=1,
                           original="x", modified="y", diff="---",
                           estimated_impact=v)
            assert s.estimated_impact == v
        with pytest.raises(Exception):
            Suggestion(description="d", reasoning="r",
                       source_file="f.c", line_start=1, line_end=1,
                       original="x", modified="y", diff="---",
                       estimated_impact="critical")
