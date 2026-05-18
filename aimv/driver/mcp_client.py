# [AIMV] AIMV Driver — MCP REST client with retry
import httpx
import time
from typing import Optional


class MCPClient:
    """MCP analysis service REST client.

    Features:
      - Exponential backoff retry (max 2 retries)
      - Total timeout budget: 180s (including retries)
      - 401/403 → PermissionError, 422 → ValueError
      - Connection health check
    """

    def __init__(self, base_url: str, timeout_seconds: int = 60,
                 api_key: Optional[str] = None):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout_seconds
        headers = {}
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        self._client = httpx.Client(timeout=timeout_seconds + 10, headers=headers)

    def analyze(self, request_json: dict) -> Optional[dict]:
        """POST /api/v1/analyze-vectorization, return parsed response dict.

        Retry strategy: 3 attempts with exponential backoff (2s, 4s).
        Total timeout budget: 180s (including retry waits).
        """
        url = f"{self.base_url}/api/v1/analyze-vectorization"
        deadline = time.monotonic() + 180

        for attempt in range(3):
            if time.monotonic() > deadline:
                return None  # total timeout

            try:
                start = time.monotonic()
                resp = self._client.post(url, json=request_json)

                if resp.status_code == 200:
                    return resp.json()
                elif resp.status_code in (401, 403):
                    raise PermissionError(
                        f"MCP authentication failed (HTTP {resp.status_code}): "
                        f"check AIMV_MCP_API_KEY configuration")
                elif resp.status_code == 422:
                    raise ValueError(f"MCP validation error: {resp.text}")
                elif resp.status_code in (429, 500, 502, 503):
                    if attempt < 2:
                        wait = 2.0 * (2 ** attempt)
                        time.sleep(wait)
                        continue
                    return None
                else:
                    raise RuntimeError(
                        f"MCP unexpected HTTP {resp.status_code}: {resp.text[:200]}")

            except (httpx.TimeoutException, httpx.ConnectError):
                if attempt < 2:
                    wait = 2.0 * (2 ** attempt)
                    time.sleep(wait)
                    continue
                return None

        return None

    def health(self) -> bool:
        """GET /api/v1/health"""
        try:
            resp = self._client.get(f"{self.base_url}/api/v1/health")
            return resp.status_code == 200
        except Exception:
            return False
