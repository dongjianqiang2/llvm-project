# [AIMV] T4.3 — MockLLMBackend tests
import pytest

from aimv.mcp_server.llm.mock_backend import MockLLMBackend, create_mock_backend
from aimv.mcp_server.models import (
    AnalyzeRequest, AnalyzeResponse, SingleDiagnostic, TargetInfo,
    FunctionInfo, RemarkSeverity, AimvLevel,
)


def _make_request(remark_id="CantReorderMemOps", severity=RemarkSeverity.MISSED,
                  source_code="void foo(int *a, int *b) { for(int i=0;i<10;i++) a[i]=b[i]; }\n"):
    return AnalyzeRequest(
        request_id="test-1",
        target=TargetInfo(triple="armv7-unknown-linux-gnueabi", cpu="cortex-a9",
                          features=["neon"], vector_width=128),
        function=FunctionInfo(
            name="foo", signature="void foo(int *a, int *b)",
            source_code=source_code, source_file="test.c", loop_line=1,
        ),
        diagnostics=[SingleDiagnostic(
            pass_name="LoopVectorize", remark_id=remark_id,
            remark_text="test", severity=severity,
            function_name="foo", loop_location="test.c:1:1",
            source_context="", ir_snippet="",
        )],
        aimv_level=AimvLevel.CONSERVATIVE,
    )


class TestMockBackendAlias:
    def test_mock_backend_alias(self):
        """CantReorderMemOps → suggestion to add restrict."""
        backend = MockLLMBackend()
        req = _make_request("CantReorderMemOps")
        resp = backend.analyze(req)
        assert isinstance(resp, AnalyzeResponse)
        assert resp.no_action_possible is False
        assert len(resp.suggestions) >= 1
        assert "restrict" in resp.suggestions[0].description.lower()

    def test_mock_backend_unsafe_dep(self):
        """UnsafeDep → suggestion about restrict/restructure."""
        backend = MockLLMBackend()
        req = _make_request("UnsafeDep")
        resp = backend.analyze(req)
        assert resp.no_action_possible is False
        assert len(resp.suggestions) >= 1

    def test_mock_backend_unknown(self):
        """Unknown remark_id → no_action_possible=True."""
        backend = MockLLMBackend()
        req = _make_request("UnknownRemarkId")
        resp = backend.analyze(req)
        assert resp.no_action_possible is True
        assert len(resp.suggestions) == 0

    def test_mock_backend_passed_severity_ignored(self):
        """Passed severity diagnostics → no suggestion (only missed analyzed)."""
        backend = MockLLMBackend()
        req = _make_request("CantReorderMemOps", severity=RemarkSeverity.PASSED)
        resp = backend.analyze(req)
        assert resp.no_action_possible is True

    def test_mock_backend_health_check(self):
        """health_check always returns True."""
        backend = MockLLMBackend()
        assert backend.health_check() is True

    def test_create_mock_backend_factory(self):
        """Factory function creates MockLLMBackend instance."""
        backend = create_mock_backend()
        assert isinstance(backend, MockLLMBackend)

    def test_mock_backend_diff_has_source_file(self):
        """Generated diff contains the source file path."""
        backend = MockLLMBackend()
        req = _make_request("CantReorderMemOps")
        resp = backend.analyze(req)
        if resp.suggestions:
            assert "test.c" in resp.suggestions[0].diff

    def test_mock_backend_confidence(self):
        """Mock backend returns reasonable confidence."""
        backend = MockLLMBackend()
        req = _make_request("CantReorderMemOps")
        resp = backend.analyze(req)
        if not resp.no_action_possible:
            assert resp.confidence > 0
            assert resp.confidence <= 1.0

    def test_mock_backend_multiple_diagnostics(self):
        """Multiple missed diagnostics → at most 1 suggestion returned."""
        backend = MockLLMBackend()
        from aimv.mcp_server.models import AnalyzeRequest as AR
        req = AR(
            request_id="test-multi",
            target=TargetInfo(triple="armv7", cpu="", features=[], vector_width=128),
            function=FunctionInfo(
                name="foo", signature="void foo()",
                source_code="void foo(){}", source_file="test.c", loop_line=1,
            ),
            diagnostics=[
                SingleDiagnostic(
                    pass_name="LoopVectorize", remark_id="CantReorderMemOps",
                    remark_text="test", severity=RemarkSeverity.MISSED,
                    function_name="foo", loop_location="test.c:1:1",
                    source_context="", ir_snippet="",
                ),
                SingleDiagnostic(
                    pass_name="LoopVectorize", remark_id="UnsafeDep",
                    remark_text="test2", severity=RemarkSeverity.MISSED,
                    function_name="foo", loop_location="test.c:2:1",
                    source_context="", ir_snippet="",
                ),
            ],
            aimv_level=AimvLevel.CONSERVATIVE,
        )
        resp = backend.analyze(req)
        assert len(resp.suggestions) <= 1
