# [AIMV] AIMV Driver — MCP REST client with retry
import httpx
import time
from typing import Optional


class MCPClient:
    def __init__(self, base_url: str, timeout_seconds: int = 60):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout_seconds
        self._client = httpx.Client(timeout=timeout_seconds + 10)

    def analyze(self, request_json: dict) -> Optional[dict]:
        url = f"{self.base_url}/api/v1/analyze-vectorization"
        for attempt in range(3):
            try:
                resp = self._client.post(url, json=request_json)
                if resp.status_code == 200:
                    return resp.json()
                elif resp.status_code == 422:
                    raise ValueError(f"MCP validation error: {resp.text}")
                elif resp.status_code in (429, 500, 502, 503):
                    if attempt < 2:
                        time.sleep(2.0 * (2 ** attempt))
                        continue
                    return None
                else:
                    return None
            except (httpx.TimeoutException, httpx.ConnectError):
                if attempt < 2:
                    time.sleep(2.0 * (2 ** attempt))
                    continue
                return None
        return None

    def health(self) -> bool:
        try:
            resp = self._client.get(f"{self.base_url}/api/v1/health")
            return resp.status_code == 200
        except Exception:
            return False
