# [AIMV] MCP Server — API key auth middleware
import os
from fastapi import Request, HTTPException
from starlette.middleware.base import BaseHTTPMiddleware

API_KEY = os.environ.get("AIMV_API_KEY", "")


class APIKeyMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        if request.url.path.startswith("/api/v1/"):
            if API_KEY:
                auth = request.headers.get("Authorization", "")
                if not auth.startswith("Bearer ") or \
                   auth.split(" ", 1)[1] != API_KEY:
                    raise HTTPException(
                        status_code=401, detail="Invalid API key")
        return await call_next(request)
