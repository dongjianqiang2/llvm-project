"""T2.5 — Diagnostic cache tests."""
import sys
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.mcp_server.cache import DiagnosticCache, compute_diagnostic_fingerprint
from aimv.mcp_server.models import (
    AnalyzeRequest, TargetInfo, FunctionInfo, SingleDiagnostic,
    RemarkSeverity, AnalyzeResponse,
)


VALID_TARGET = TargetInfo(triple="arm", cpu="cortex-a9", features=[], vector_width=128)
VALID_FUNC1 = FunctionInfo(name="foo", signature="void f()",
                           source_code="void f(){}", source_file="f.c", loop_line=1)
VALID_FUNC2 = FunctionInfo(name="foo", signature="void f()",
                           source_code="void f(){}", source_file="f.c", loop_line=1)
VALID_FUNC_OTHER = FunctionInfo(name="foo", signature="void f()",
                                source_code="void g(){}", source_file="g.c", loop_line=1)
VALID_DIAG = SingleDiagnostic(
    pass_name="LoopVectorize", remark_id="CantReorderMemOps",
    remark_text="loop not vectorized", severity=RemarkSeverity.MISSED,
    function_name="foo", loop_location="f.c:1:1",
    source_context="", ir_snippet="")

VALID_RESPONSE = AnalyzeResponse(
    request_id="r1", suggestions=[],
    overall_analysis="ok", confidence=1.0)


@pytest.fixture
def cache():
    return DiagnosticCache(ttl_seconds=3600)


@pytest.fixture
def request1():
    return AnalyzeRequest(
        request_id="r1", target=VALID_TARGET,
        function=VALID_FUNC1, diagnostics=[VALID_DIAG])


class TestFingerprint:
    def test_same_request_same_fingerprint(self, request1):
        f1 = compute_diagnostic_fingerprint(request1)
        f2 = compute_diagnostic_fingerprint(request1)
        assert f1 == f2

    def test_different_source_different_fingerprint(self):
        req1 = AnalyzeRequest(
            request_id="r1", target=VALID_TARGET,
            function=VALID_FUNC1, diagnostics=[VALID_DIAG])
        req2 = AnalyzeRequest(
            request_id="r2", target=VALID_TARGET,
            function=VALID_FUNC_OTHER, diagnostics=[VALID_DIAG])
        assert compute_diagnostic_fingerprint(req1) != compute_diagnostic_fingerprint(req2)


class TestDiagnosticCache:
    def test_set_and_get(self, cache, request1):
        fingerprint = compute_diagnostic_fingerprint(request1)
        cache.set(fingerprint, VALID_RESPONSE)
        result = cache.get(fingerprint)
        assert result is not None
        assert result.request_id == "r1"

    def test_expired_ttl_returns_none(self, request1):
        cache = DiagnosticCache(ttl_seconds=0)  # immediate expiry
        fingerprint = compute_diagnostic_fingerprint(request1)
        cache.set(fingerprint, VALID_RESPONSE)
        import time
        time.sleep(0.01)
        result = cache.get(fingerprint)
        assert result is None

    def test_stats(self, cache, request1):
        fingerprint = compute_diagnostic_fingerprint(request1)
        cache.set(fingerprint, VALID_RESPONSE)
        cache.get(fingerprint)  # hit
        cache.get("nonexistent")  # miss
        stats = cache.get_stats()
        assert stats["total_requests"] >= 2
        assert stats["cache_hits"] >= 1
        assert stats["cache_misses"] >= 1
        assert 0 <= stats["hit_rate"] <= 1
