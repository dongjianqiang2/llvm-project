# [AIMV] Tests for aimv/mcp_server/cache.py (T2.6)
import time
import pytest
from aimv.mcp_server.cache import DiagnosticCache, compute_diagnostic_fingerprint
from aimv.mcp_server.models import (
    AnalyzeRequest, TargetInfo, FunctionInfo, SingleDiagnostic,
    RemarkSeverity, AnalyzeResponse, HistoryRecord,
)

VALID_TARGET = TargetInfo(triple="arm", cpu="cortex-a9", features=["neon"], vector_width=128)
VALID_FUNC1 = FunctionInfo(name="foo", signature="void f()",
                           source_code="void f(){}", source_file="f.c", loop_line=1)
VALID_FUNC_OTHER = FunctionInfo(name="bar", signature="void g()",
                                source_code="void g(){ int x=1; }", source_file="g.c", loop_line=1)
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


class TestFingerprintStrict:
    """T2.6: Strict mode includes source_code hash + history."""

    def test_same_request_same_fingerprint(self):
        req = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                             function=VALID_FUNC1, diagnostics=[VALID_DIAG])
        f1 = compute_diagnostic_fingerprint(req, mode="strict")
        f2 = compute_diagnostic_fingerprint(req, mode="strict")
        assert f1 == f2
        assert f1.startswith("strict:")

    def test_different_source_different_fingerprint(self):
        req1 = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                              function=VALID_FUNC1, diagnostics=[VALID_DIAG])
        req2 = AnalyzeRequest(request_id="r2", target=VALID_TARGET,
                              function=VALID_FUNC_OTHER, diagnostics=[VALID_DIAG])
        assert compute_diagnostic_fingerprint(req1) != compute_diagnostic_fingerprint(req2)


class TestFingerprintRelaxed:
    """T2.6: Relaxed mode: only target + diagnostics, cross-function reuse."""

    def test_relaxed_prefix(self):
        req = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                             function=VALID_FUNC1, diagnostics=[VALID_DIAG])
        fp = compute_diagnostic_fingerprint(req, mode="relaxed")
        assert fp.startswith("relaxed:")

    def test_relaxed_different_source_same_fingerprint(self):
        """Different source_code but same diagnostics pattern → same relaxed fingerprint."""
        func_a = FunctionInfo(name="foo_a", signature="void fa()",
                              source_code="void fa(){ x++; }", source_file="a.c", loop_line=1)
        func_b = FunctionInfo(name="foo_b", signature="void fb()",
                              source_code="void fb(){ y++; }", source_file="b.c", loop_line=1)
        diag_a = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="CantReorderMemOps",
            remark_text="loop not vectorized", severity=RemarkSeverity.MISSED,
            function_name="foo_a", loop_location="a.c:1:1",
            source_context="", ir_snippet="")
        diag_b = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="CantReorderMemOps",
            remark_text="loop not vectorized", severity=RemarkSeverity.MISSED,
            function_name="foo_b", loop_location="b.c:1:1",
            source_context="", ir_snippet="")
        req_a = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                               function=func_a, diagnostics=[diag_a])
        req_b = AnalyzeRequest(request_id="r2", target=VALID_TARGET,
                               function=func_b, diagnostics=[diag_b])
        # Relaxed: same remark_id + remark_text prefix → same fingerprint
        fp_a = compute_diagnostic_fingerprint(req_a, mode="relaxed")
        fp_b = compute_diagnostic_fingerprint(req_b, mode="relaxed")
        assert fp_a == fp_b
        # Strict: different source_code → different fingerprint
        assert compute_diagnostic_fingerprint(req_a) != compute_diagnostic_fingerprint(req_b)


class TestDiagnosticCache:
    def test_cache_strict_hit(self, cache):
        req = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                             function=VALID_FUNC1, diagnostics=[VALID_DIAG])
        fp = compute_diagnostic_fingerprint(req)
        cache.set(fp, VALID_RESPONSE)
        result = cache.get(fp)
        assert result is not None
        assert result.request_id == "r1"

    def test_cache_strict_miss_different_source(self, cache):
        req1 = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                              function=VALID_FUNC1, diagnostics=[VALID_DIAG])
        req2 = AnalyzeRequest(request_id="r2", target=VALID_TARGET,
                              function=VALID_FUNC_OTHER, diagnostics=[VALID_DIAG])
        fp1 = compute_diagnostic_fingerprint(req1)
        fp2 = compute_diagnostic_fingerprint(req2)
        cache.set(fp1, VALID_RESPONSE)
        assert cache.get(fp2) is None

    def test_cache_ttl_expiry(self):
        cache = DiagnosticCache(ttl_seconds=0)
        req = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                             function=VALID_FUNC1, diagnostics=[VALID_DIAG])
        fp = compute_diagnostic_fingerprint(req)
        cache.set(fp, VALID_RESPONSE)
        time.sleep(0.01)
        assert cache.get(fp) is None

    def test_cache_stats(self, cache):
        req = AnalyzeRequest(request_id="r1", target=VALID_TARGET,
                             function=VALID_FUNC1, diagnostics=[VALID_DIAG])
        fp = compute_diagnostic_fingerprint(req)
        cache.set(fp, VALID_RESPONSE)
        cache.get(fp)   # hit
        cache.get("nonexistent")  # miss
        stats = cache.get_stats()
        assert stats["total_requests"] >= 2
        assert stats["cache_hits"] >= 1
        assert stats["cache_misses"] >= 1
        assert 0 <= stats["hit_rate"] <= 1
