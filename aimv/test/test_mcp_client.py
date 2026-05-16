"""T3.5 — MCPClient tests."""
import sys
import pytest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.mcp_client import MCPClient


class TestMCPClient:
    def test_analyze_success(self):
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 200
        mock_resp.json.return_value = {"request_id": "r1", "suggestions": []}
        with mock.patch.object(client._client, "post", return_value=mock_resp):
            result = client.analyze({"test": True})
            assert result == {"request_id": "r1", "suggestions": []}

    def test_analyze_422_raises_value_error(self):
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 422
        mock_resp.text = "validation error"
        with mock.patch.object(client._client, "post", return_value=mock_resp):
            with pytest.raises(ValueError, match="validation error"):
                client.analyze({})

    def test_analyze_retry_then_success(self):
        client = MCPClient("http://localhost:8080", timeout_seconds=60)
        fail_resp = mock.MagicMock()
        fail_resp.status_code = 503
        ok_resp = mock.MagicMock()
        ok_resp.status_code = 200
        ok_resp.json.return_value = {"request_id": "r1", "suggestions": []}
        with mock.patch.object(client._client, "post",
                               side_effect=[fail_resp, fail_resp, ok_resp]):
            result = client.analyze({})
            assert result is not None

    def test_analyze_all_retries_fail_returns_none(self):
        client = MCPClient("http://localhost:8080", timeout_seconds=60)
        fail_resp = mock.MagicMock()
        fail_resp.status_code = 503
        with mock.patch.object(client._client, "post",
                               side_effect=[fail_resp, fail_resp, fail_resp]):
            result = client.analyze({})
            assert result is None

    def test_health_ok(self):
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 200
        with mock.patch.object(client._client, "get", return_value=mock_resp):
            assert client.health() is True

    def test_health_fail(self):
        client = MCPClient("http://localhost:8080")
        mock_resp = mock.MagicMock()
        mock_resp.status_code = 500
        with mock.patch.object(client._client, "get", return_value=mock_resp):
            assert client.health() is False
