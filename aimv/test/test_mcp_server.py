# [AIMV] Tests for aimv/mcp_server/aimv_server.py + middleware.py (T2.2, T2.7)
import json
import os
import pytest
from unittest.mock import patch
from fastapi.testclient import TestClient
from aimv.mcp_server.models import (
    AnalyzeRequest, TargetInfo, FunctionInfo, SingleDiagnostic,
    RemarkSeverity, AnalyzeResponse,
)


VALID_REQUEST_BODY = {
    "request_id": "test-1",
    "target": {"triple": "armv7-unknown-linux-gnueabi", "cpu": "cortex-a9",
               "features": ["neon"], "vector_width": 128},
    "function": {"name": "foo", "signature": "void foo()",
                 "source_code": "void foo(){}", "source_file": "f.c",
                 "loop_line": 1},
    "diagnostics": [{
        "pass_name": "LoopVectorize", "remark_id": "CantReorderMemOps",
        "remark_text": "failed", "severity": "missed",
        "function_name": "foo", "loop_location": "f.c:1:1",
        "source_context": "", "ir_snippet": "",
    }],
    "aimv_level": "conservative",
}


def _make_client(api_key=""):
    """Create TestClient with specific API key by patching _get_api_key."""
    import aimv.mcp_server.middleware as mw
    from aimv.mcp_server.aimv_server import app
    client = TestClient(app)
    # Store the api_key for the middleware to use
    client._aimv_api_key = api_key
    return client


class TestHealthEndpoint:
    def test_health_no_auth(self):
        client = _make_client(api_key="")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.get("/api/v1/health")
        assert resp.status_code == 200
        data = resp.json()
        assert data["status"] == "ok"

    def test_health_with_auth_no_key(self):
        client = _make_client(api_key="test-secret-key")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value="test-secret-key"):
            resp = client.get("/api/v1/health")
        assert resp.status_code == 401

    def test_health_with_auth_wrong_key(self):
        client = _make_client(api_key="test-secret-key")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value="test-secret-key"):
            resp = client.get("/api/v1/health",
                              headers={"Authorization": "Bearer wrong-key"})
        assert resp.status_code == 401

    def test_health_with_auth_valid_key(self):
        client = _make_client(api_key="test-secret-key")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value="test-secret-key"):
            resp = client.get("/api/v1/health",
                              headers={"Authorization": "Bearer test-secret-key"})
        assert resp.status_code == 200


class TestAnalyzeEndpoint:
    def test_analyze_no_auth(self):
        client = _make_client(api_key="")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/analyze-vectorization", json=VALID_REQUEST_BODY)
        assert resp.status_code == 200
        data = resp.json()
        # MockLLMBackend recognizes CantReorderMemOps and returns a suggestion
        assert data["no_action_possible"] is False
        assert len(data["suggestions"]) >= 1

    def test_analyze_invalid_request(self):
        client = _make_client(api_key="")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/analyze-vectorization", json={"bad": "data"})
        assert resp.status_code == 422

    def test_analyze_with_auth_invalid_key(self):
        client = _make_client(api_key="test-secret-key")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value="test-secret-key"):
            resp = client.post("/api/v1/analyze-vectorization",
                              json=VALID_REQUEST_BODY,
                              headers={"Authorization": "Bearer wrong"})
        assert resp.status_code == 401

    def test_analyze_with_auth_valid_key(self):
        client = _make_client(api_key="test-secret-key")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value="test-secret-key"):
            resp = client.post("/api/v1/analyze-vectorization",
                              json=VALID_REQUEST_BODY,
                              headers={"Authorization": "Bearer test-secret-key"})
        assert resp.status_code == 200


class TestCacheStatsEndpoint:
    def test_cache_stats(self):
        client = _make_client(api_key="")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.get("/api/v1/cache/stats")
        assert resp.status_code == 200
        data = resp.json()
        assert "hit_rate" in data


class TestFeedbackEndpoint:
    def test_feedback(self):
        client = _make_client(api_key="")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value=""):
            resp = client.post("/api/v1/feedback", json={
                "pattern": "CantReorderMemOps",
                "strategy": "add_restrict",
                "result": "success",
            })
        assert resp.status_code == 200


class TestAPIKeyMiddleware:
    """T2.7: APIKeyMiddleware returns JSONResponse, not HTTPException."""

    def test_api_key_missing_returns_401_json(self):
        client = _make_client(api_key="test-secret-key")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value="test-secret-key"):
            resp = client.get("/api/v1/health")
        assert resp.status_code == 401
        assert resp.json()["detail"] == "Invalid API key"

    def test_api_key_invalid_returns_401_json(self):
        client = _make_client(api_key="test-secret-key")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value="test-secret-key"):
            resp = client.get("/api/v1/health",
                              headers={"Authorization": "Bearer wrong"})
        assert resp.status_code == 401
        assert resp.json()["detail"] == "Invalid API key"

    def test_non_api_path_bypasses_auth(self):
        client = _make_client(api_key="test-secret-key")
        with patch("aimv.mcp_server.middleware._get_api_key", return_value="test-secret-key"):
            resp = client.get("/docs")
        assert resp.status_code != 401
