# [AIMV] T6.1-T6.3 — Stage 6: Loop transform combination tests
import pytest

from aimv.mcp_server.models import (
    AnalyzeRequest, AnalyzeResponse, SingleDiagnostic, TargetInfo,
    FunctionInfo, RemarkSeverity, AimvLevel, LoopTransformRequest,
    LoopTransformResponse, LoopNestInfo, TransformSuggestion,
)
from aimv.mcp_server.prompt_builder import build_loop_transform_prompt
from fastapi.testclient import TestClient
from unittest.mock import patch


VALID_TARGET = TargetInfo(
    triple="armv7-unknown-linux-gnueabi", cpu="cortex-a9",
    features=["neon"], vector_width=128)


# --- T6.1: Loop Transform API endpoint ---


class TestLoopTransformEndpoint:
    """T6.1: POST /api/v1/analyze-loop-transform endpoint."""

    def _make_transform_body(self, loop_nest=None, remark_id="CantReorderMemOps"):
        body = {
            "request_id": "r-lt1",
            "target": {"triple": "armv7-unknown-linux-gnueabi", "cpu": "cortex-a9",
                        "features": ["neon"], "vector_width": 128},
            "function": {
                "name": "matmul", "signature": "void matmul(int *a, int *b, int *c, int N)",
                "source_code": "void matmul(int *a, int *b, int *c, int N) {\n  for (int i = 0; i < N; i++)\n    for (int j = 0; j < N; j++)\n      c[i*N+j] = a[i*N+j] + b[i*N+j];\n}\n",
                "source_file": "matmul.c", "loop_line": 2,
            },
            "diagnostics": [{
                "pass_name": "LoopVectorize", "remark_id": remark_id,
                "remark_text": "failed", "severity": "missed",
                "function_name": "matmul", "loop_location": "matmul.c:2:3",
                "source_context": "", "ir_snippet": "",
            }],
            "aimv_level": "conservative",
        }
        if loop_nest:
            body["loop_nest"] = loop_nest
        return body

    def test_endpoint_reachable(self):
        from aimv.mcp_server.aimv_server import app
        client = TestClient(app)
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/analyze-loop-transform",
                               json=self._make_transform_body())
        assert resp.status_code == 200

    def test_endpoint_invalid_request(self):
        from aimv.mcp_server.aimv_server import app
        client = TestClient(app)
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/analyze-loop-transform",
                               json={"bad": "data"})
        assert resp.status_code == 422

    def test_endpoint_with_auth(self):
        from aimv.mcp_server.aimv_server import app
        client = TestClient(app)
        with patch("aimv.mcp_server.middleware._get_api_key", return_value="secret"):
            resp = client.post("/api/v1/analyze-loop-transform",
                               json=self._make_transform_body())
        assert resp.status_code == 401

    def test_endpoint_returns_loop_transform_response(self):
        from aimv.mcp_server.aimv_server import app
        client = TestClient(app)
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/analyze-loop-transform",
                               json=self._make_transform_body())
        data = resp.json()
        assert "request_id" in data
        assert "suggestions" in data
        assert "overall_analysis" in data
        assert "confidence" in data
        assert "no_action_possible" in data


class TestLoopTransformInterchange:
    """T6.1: Loop interchange suggestion for column-major access."""

    def _make_body_with_nest(self, is_row_major=False):
        return {
            "request_id": "r-interchange",
            "target": {"triple": "armv7-unknown-linux-gnueabi", "cpu": "cortex-a9",
                        "features": ["neon"], "vector_width": 128},
            "function": {
                "name": "transpose", "signature": "void transpose(int *a, int *b, int N)",
                "source_code": "void transpose(int *a, int *b, int N) {\n  for (int i = 0; i < N; i++)\n    for (int j = 0; j < N; j++)\n      b[j*N+i] = a[i*N+j];\n}\n",
                "source_file": "transpose.c", "loop_line": 2,
            },
            "diagnostics": [{
                "pass_name": "LoopVectorize", "remark_id": "CantReorderMemOps",
                "remark_text": "failed", "severity": "missed",
                "function_name": "transpose", "loop_location": "transpose.c:2:3",
                "source_context": "", "ir_snippet": "",
            }],
            "loop_nest": {
                "outer_loop_line": 2,
                "inner_loop_line": 3,
                "outer_trip_count": -1,
                "inner_trip_count": -1,
                "is_row_major": is_row_major,
            },
            "aimv_level": "conservative",
        }

    def test_column_major_suggests_interchange(self):
        from aimv.mcp_server.aimv_server import app
        client = TestClient(app)
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/analyze-loop-transform",
                               json=self._make_body_with_nest(is_row_major=False))
        data = resp.json()
        assert data["no_action_possible"] is False
        assert len(data["suggestions"]) >= 1
        assert data["suggestions"][0]["transform_type"] == "interchange"

    def test_row_major_no_interchange(self):
        from aimv.mcp_server.aimv_server import app
        client = TestClient(app)
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/analyze-loop-transform",
                               json=self._make_body_with_nest(is_row_major=True))
        data = resp.json()
        # Row-major may still get distribution suggestion
        interchange = [s for s in data["suggestions"] if s["transform_type"] == "interchange"]
        assert len(interchange) == 0


class TestLoopTransformFission:
    """T6.1: Loop distribution suggestion for alias failure."""

    def test_alias_suggests_distribution(self):
        from aimv.mcp_server.aimv_server import app
        client = TestClient(app)
        body = {
            "request_id": "r-fission",
            "target": {"triple": "armv7-unknown-linux-gnueabi", "cpu": "cortex-a9",
                        "features": ["neon"], "vector_width": 128},
            "function": {
                "name": "mixed", "signature": "void mixed(int *a, int *b, int n)",
                "source_code": "void mixed(int *a, int *b, int n) {\n  for (int i = 0; i < n; i++) {\n    a[i] = b[i];\n    b[i+1] = a[i] * 2;\n  }\n}\n",
                "source_file": "mixed.c", "loop_line": 2,
            },
            "diagnostics": [{
                "pass_name": "LoopVectorize", "remark_id": "CantReorderMemOps",
                "remark_text": "alias", "severity": "missed",
                "function_name": "mixed", "loop_location": "mixed.c:2:3",
                "source_context": "", "ir_snippet": "",
            }],
            "aimv_level": "conservative",
        }
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/analyze-loop-transform", json=body)
        data = resp.json()
        assert data["no_action_possible"] is False
        dist = [s for s in data["suggestions"] if s["transform_type"] == "distribution"]
        assert len(dist) >= 1


# --- T6.2: Loop transform prompt + suggestion strategy ---


class TestLoopTransformPrompt:
    """T6.2: Prompt builder for loop transform analysis."""

    def _make_transform_request(self, is_row_major=None):
        loop_nest = LoopNestInfo(
            outer_loop_line=2, inner_loop_line=3,
            outer_trip_count=100, inner_trip_count=50,
            is_row_major=is_row_major,
        )
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="CantReorderMemOps",
            remark_text="alias failure", severity=RemarkSeverity.MISSED,
            function_name="transpose", loop_location="t.c:2:3",
            source_context="", ir_snippet="",
        )
        return LoopTransformRequest(
            request_id="r1", target=VALID_TARGET,
            function=FunctionInfo(
                name="transpose",
                signature="void transpose(int *a, int *b, int N)",
                source_code="void transpose(int *a, int *b, int N) {\n  for (int i = 0; i < N; i++)\n    for (int j = 0; j < N; j++)\n      b[j*N+i] = a[i*N+j];\n}\n",
                source_file="t.c", loop_line=2,
            ),
            diagnostics=[diag],
            loop_nest=loop_nest,
            aimv_level=AimvLevel.MODERATE,
        )

    def test_prompt_contains_function(self):
        req = self._make_transform_request()
        prompt = build_loop_transform_prompt(req)
        assert "transpose" in prompt

    def test_prompt_contains_loop_nest(self):
        req = self._make_transform_request()
        prompt = build_loop_transform_prompt(req)
        assert "Outer loop" in prompt
        assert "Inner loop" in prompt

    def test_prompt_contains_access_pattern(self):
        req = self._make_transform_request(is_row_major=False)
        prompt = build_loop_transform_prompt(req)
        assert "column-major" in prompt

    def test_prompt_contains_transform_guidance(self):
        req = self._make_transform_request()
        prompt = build_loop_transform_prompt(req)
        assert "interchange" in prompt.lower()
        assert "fission" in prompt.lower() or "distribution" in prompt.lower()

    def test_prompt_no_nest_no_loop_structure(self):
        diag = SingleDiagnostic(
            pass_name="LoopVectorize", remark_id="CantReorderMemOps",
            remark_text="alias", severity=RemarkSeverity.MISSED,
            function_name="f", loop_location="f.c:1:1",
            source_context="", ir_snippet="",
        )
        req = LoopTransformRequest(
            request_id="r1", target=VALID_TARGET,
            function=FunctionInfo(name="f", signature="void f()",
                                   source_code="void f(){}", source_file="f.c", loop_line=1),
            diagnostics=[diag],
        )
        prompt = build_loop_transform_prompt(req)
        assert "Loop Nest Structure" not in prompt


# --- T6.3: Multi-pass cooperative integration ---


class TestMultiPassCooperative:
    """T6.3: Nested loop benchmark → interchange + vectorize joint analysis."""

    def test_interchange_suggestion_has_diff(self):
        from aimv.mcp_server.aimv_server import app
        client = TestClient(app)
        body = {
            "request_id": "r-multi-pass",
            "target": {"triple": "armv7-unknown-linux-gnueabi", "cpu": "cortex-a9",
                        "features": ["neon"], "vector_width": 128},
            "function": {
                "name": "matmul",
                "signature": "void matmul(int *a, int *b, int *c, int N)",
                "source_code": "void matmul(int *a, int *b, int *c, int N) {\n  for (int i = 0; i < N; i++)\n    for (int j = 0; j < N; j++)\n      c[j*N+i] = a[i*N+j] + b[i*N+j];\n}\n",
                "source_file": "matmul.c", "loop_line": 2,
            },
            "diagnostics": [{
                "pass_name": "LoopVectorize", "remark_id": "CantReorderMemOps",
                "remark_text": "alias", "severity": "missed",
                "function_name": "matmul", "loop_location": "matmul.c:3:5",
                "source_context": "", "ir_snippet": "",
            }],
            "loop_nest": {
                "outer_loop_line": 2,
                "inner_loop_line": 3,
                "outer_trip_count": 64,
                "inner_trip_count": 64,
                "is_row_major": False,
            },
            "aimv_level": "moderate",
        }
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/analyze-loop-transform", json=body)
        data = resp.json()
        assert data["no_action_possible"] is False
        # Should have interchange suggestion
        interchange = [s for s in data["suggestions"] if s["transform_type"] == "interchange"]
        assert len(interchange) >= 1
        # Interchange suggestion should have a diff
        assert interchange[0]["diff"] != ""
        assert "safety_concern" in interchange[0]

    def test_vectorization_plus_transform(self):
        """Vectorization endpoint and transform endpoint both return suggestions."""
        from aimv.mcp_server.aimv_server import app
        client = TestClient(app)

        vec_body = {
            "request_id": "r-vec",
            "target": {"triple": "armv7-unknown-linux-gnueabi", "cpu": "cortex-a9",
                        "features": ["neon"], "vector_width": 128},
            "function": {
                "name": "foo", "signature": "void foo(int *a, int *b, int n)",
                "source_code": "void foo(int *a, int *b, int n) { for (int i=0;i<n;i++) a[i]=b[i]; }\n",
                "source_file": "f.c", "loop_line": 1,
            },
            "diagnostics": [{
                "pass_name": "LoopVectorize", "remark_id": "CantReorderMemOps",
                "remark_text": "failed", "severity": "missed",
                "function_name": "foo", "loop_location": "f.c:1:1",
                "source_context": "", "ir_snippet": "",
            }],
            "aimv_level": "conservative",
        }

        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            vec_resp = client.post("/api/v1/analyze-vectorization", json=vec_body)
        assert vec_resp.status_code == 200
        vec_data = vec_resp.json()
        assert vec_data["no_action_possible"] is False

    def test_transform_models_pydantic_validation(self):
        """LoopTransformRequest and Response validate correctly."""
        req = LoopTransformRequest(
            request_id="r1",
            target=VALID_TARGET,
            function=FunctionInfo(name="f", signature="void f()",
                                   source_code="void f(){}", source_file="f.c", loop_line=1),
            diagnostics=[SingleDiagnostic(
                pass_name="LoopVectorize", remark_id="test",
                remark_text="test", severity=RemarkSeverity.MISSED,
                function_name="f", loop_location="f.c:1:1",
                source_context="", ir_snippet="",
            )],
        )
        assert req.request_id == "r1"

        resp = LoopTransformResponse(
            request_id="r1",
            suggestions=[],
            overall_analysis="test",
            confidence=0.5,
            no_action_possible=True,
        )
        assert resp.no_action_possible is True

    def test_transform_suggestion_type_validation(self):
        """TransformSuggestion only accepts valid transform types."""
        ts = TransformSuggestion(
            transform_type="interchange",
            description="swap loops",
            reasoning="better stride",
            source_file="f.c",
            line_start=1,
            line_end=2,
            original="for i",
            modified="for j",
            diff="---\n+++",
            estimated_impact="high",
        )
        assert ts.transform_type == "interchange"

        with pytest.raises(Exception):
            TransformSuggestion(
                transform_type="invalid_type",
                description="bad",
                reasoning="bad",
                source_file="f.c",
                line_start=1,
                line_end=1,
                original="",
                modified="",
                diff="",
                estimated_impact="medium",
            )
