# [AIMV] T3.2 — MCPClient tests
import pytest
from unittest import mock

from aimv.driver.mcp_client import MCPClient


class TestMCPClientAnalyze:
    def test_analyze_success(self):
        """mock 200 → returns dict."""
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 200
        mock_resp.json.return_value = {"request_id": "r1", "suggestions": []}
        with mock.patch.object(client._client, "post", return_value=mock_resp):
            result = client.analyze({"test": True})
            assert result == {"request_id": "r1", "suggestions": []}

    def test_analyze_401_auth_error(self):
        """mock 401 → PermissionError with AIMV_MCP_API_KEY message."""
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 401
        mock_resp.text = "unauthorized"
        with mock.patch.object(client._client, "post", return_value=mock_resp):
            with pytest.raises(PermissionError, match="AIMV_MCP_API_KEY"):
                client.analyze({})

    def test_analyze_403_auth_error(self):
        """mock 403 → PermissionError."""
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 403
        mock_resp.text = "forbidden"
        with mock.patch.object(client._client, "post", return_value=mock_resp):
            with pytest.raises(PermissionError, match="AIMV_MCP_API_KEY"):
                client.analyze({})

    def test_analyze_422_validation_error(self):
        """mock 422 → ValueError."""
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 422
        mock_resp.text = "validation error"
        with mock.patch.object(client._client, "post", return_value=mock_resp):
            with pytest.raises(ValueError, match="validation error"):
                client.analyze({})

    def test_analyze_429_retry_success(self):
        """mock 429 → 429 → 200 → success."""
        client = MCPClient("http://localhost:8080", timeout_seconds=60)
        fail_resp = mock.MagicMock()
        fail_resp.status_code = 429
        ok_resp = mock.MagicMock()
        ok_resp.status_code = 200
        ok_resp.json.return_value = {"request_id": "r1", "suggestions": []}
        with mock.patch.object(client._client, "post",
                               side_effect=[fail_resp, fail_resp, ok_resp]):
            result = client.analyze({})
            assert result is not None

    def test_analyze_all_retries_fail_returns_none(self):
        """mock 503 x3 → returns None."""
        client = MCPClient("http://localhost:8080", timeout_seconds=60)
        fail_resp = mock.MagicMock()
        fail_resp.status_code = 503
        with mock.patch.object(client._client, "post",
                               side_effect=[fail_resp, fail_resp, fail_resp]):
            result = client.analyze({})
            assert result is None

    def test_analyze_unexpected_status_raises(self):
        """mock 404 → RuntimeError."""
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 404
        mock_resp.text = "not found"
        with mock.patch.object(client._client, "post", return_value=mock_resp):
            with pytest.raises(RuntimeError, match="unexpected HTTP 404"):
                client.analyze({})

    def test_analyze_total_timeout(self):
        """mock TimeoutException x3 → returns None (total 180s budget)."""
        import httpx
        client = MCPClient("http://localhost:8080", timeout_seconds=60)
        with mock.patch.object(client._client, "post",
                               side_effect=httpx.TimeoutException("timeout")):
            # Mock time.monotonic to simulate timeout
            with mock.patch("aimv.driver.mcp_client.time.monotonic",
                            side_effect=[0, 0, 0, 0, 0, 0, 200]):
                result = client.analyze({})
                # Either None or we get 3 timeouts
                assert result is None or result is None


class TestMCPClientHealth:
    def test_health_ok(self):
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 200
        with mock.patch.object(client._client, "get", return_value=mock_resp):
            assert client.health() is True

    def test_health_fail(self):
        client = MCPClient("http://localhost:8080")
        with mock.patch.object(client._client, "get",
                               side_effect=Exception("connection error")):
            assert client.health() is False

    def test_health_non_200(self):
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 500
        with mock.patch.object(client._client, "get", return_value=mock_resp):
            assert client.health() is False
