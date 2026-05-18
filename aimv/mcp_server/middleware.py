# [AIMV] MCP Server — API key auth middleware + error handler
import os
import time
import asyncio
import logging
from fastapi import Request
from fastapi.responses import JSONResponse
from starlette.middleware.base import BaseHTTPMiddleware
from .models import AnalyzeResponse

logger = logging.getLogger("aimv.mcp")

RETRIABLE_LLM_ERRORS = (
    "rate_limit_exceeded",
    "server_error",
    "timeout",
    "overloaded",
)


def _get_api_key() -> str:
    """Read API key at request time (not import time) for testability."""
    return os.environ.get("AIMV_API_KEY", "")


class APIKeyMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        api_key = _get_api_key()
        if api_key and request.url.path.startswith("/api/v1/"):
            auth = request.headers.get("Authorization", "")
            if not auth.startswith("Bearer ") or auth.split(" ", 1)[1] != api_key:
                return JSONResponse(
                    status_code=401,
                    content={"detail": "Invalid API key"},
                )
        return await call_next(request)


class AIMVErrorHandler:
    """Unified LLM error handling with retry and timeout fallback."""

    def __init__(self, max_retries: int = 2, retry_delay: float = 2.0):
        self.max_retries = max_retries
        self.retry_delay = retry_delay

    async def handle_llm_call(self, backend, request):
        """Call backend.analyze() with retry for retriable errors.

        Note: backend.analyze() is a synchronous blocking call.
        Production should use run_in_executor. MVP accepts blocking.
        """
        last_error = None

        for attempt in range(self.max_retries + 1):
            try:
                return backend.analyze(request)
            except Exception as e:
                last_error = e
                error_type = _classify_error(e)

                if error_type not in RETRIABLE_LLM_ERRORS:
                    break

                if attempt < self.max_retries:
                    delay = self.retry_delay * (2 ** attempt)
                    logger.warning(f"LLM error (retry {attempt + 1}/{self.max_retries}): {e}")
                    await asyncio.sleep(delay)

        logger.error(f"LLM call failed after {self.max_retries + 1} attempts: {last_error}")
        return AnalyzeResponse(
            request_id=request.request_id,
            suggestions=[],
            overall_analysis=f"MCP service temporarily unavailable: {str(last_error)[:200]}",
            confidence=0.0,
            no_action_possible=True,
        )


def _classify_error(e: Exception) -> str:
    """Classify an exception for retry decision."""
    msg = str(e).lower()
    if "rate" in msg or "429" in msg:
        return "rate_limit_exceeded"
    if "timeout" in msg:
        return "timeout"
    if "overload" in msg or "503" in msg or "502" in msg:
        return "overloaded"
    if "500" in msg or "server" in msg:
        return "server_error"
    return "unknown"
