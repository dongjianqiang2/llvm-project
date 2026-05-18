# [AIMV] MCP Server — FastAPI entry point
"""aimv-server — MCP REST API for AI-driven vectorization analysis."""

import os
import time
import asyncio
import logging
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
from .middleware import APIKeyMiddleware, AIMVErrorHandler
from .models import AnalyzeRequest, AnalyzeResponse
from .prompt_builder import build_system_prompt, build_user_prompt
from .suggestion_parser import parse_structured_response, SuggestionParseError
from .cache import DiagnosticCache, compute_diagnostic_fingerprint

logger = logging.getLogger("aimv.mcp")

app = FastAPI(title="AIMV MCP Server", version="0.1.0")
app.add_middleware(APIKeyMiddleware)

LLM_BACKEND = os.environ.get("AIMV_LLM_BACKEND", "mock")
error_handler = AIMVErrorHandler(max_retries=2, retry_delay=2.0)

cache = DiagnosticCache(
    ttl_seconds=int(os.environ.get("AIMV_CACHE_TTL", "86400"))
)


@app.get("/api/v1/health")
async def health():
    return {
        "status": "ok",
        "backend": LLM_BACKEND,
        "model": os.environ.get("AIMV_LLM_MODEL", "mock"),
        "cache_hits": cache._cache_hits,
        "cache_misses": cache._cache_misses,
        "uptime_seconds": 0,
    }


@app.get("/api/v1/cache/stats")
async def cache_stats():
    return cache.get_stats()


@app.post("/api/v1/feedback")
async def feedback(request: Request):
    """Record iteration result for prompt optimization (Phase 2)."""
    body = await request.json()
    # MVP: just acknowledge, no storage
    return {"status": "recorded"}


@app.post("/api/v1/analyze-vectorization")
async def analyze_vectorization(request: Request):
    start = time.monotonic()

    # Parse and validate
    try:
        body = await request.json()
        analyze_req = AnalyzeRequest.model_validate(body)
    except Exception as e:
        return JSONResponse(
            status_code=422,
            content={"detail": [{"msg": str(e)}]},
        )

    # Check cache
    fingerprint = compute_diagnostic_fingerprint(analyze_req)
    cached = cache.get(fingerprint)
    if cached:
        return cached.model_dump()

    # Build prompt
    try:
        system_prompt = build_system_prompt(analyze_req)
        user_prompt = build_user_prompt(analyze_req)
    except Exception as e:
        logger.error(f"Prompt build failed: {e}")
        return AnalyzeResponse(
            request_id=analyze_req.request_id,
            suggestions=[],
            overall_analysis=f"Prompt construction error: {str(e)[:200]}",
            confidence=0.0,
            no_action_possible=True,
        ).model_dump()

    # LLM call
    if LLM_BACKEND == "mock":
        from .llm.mock_backend import MockLLMBackend
        mock_backend = MockLLMBackend()
        response = mock_backend.analyze(analyze_req)
    else:
        try:
            backend = _create_backend(LLM_BACKEND)
            if backend is None:
                return JSONResponse(
                    status_code=500,
                    content={"detail": f"Unknown LLM backend: {LLM_BACKEND}"},
                )
            # MVP: backend.analyze() is synchronous blocking call
            # Production: use asyncio.get_event_loop().run_in_executor(None, backend.analyze, analyze_req)
            response = await error_handler.handle_llm_call(backend, analyze_req)
        except Exception as e:
            logger.error(f"LLM call failed: {e}")
            response = AnalyzeResponse(
                request_id=analyze_req.request_id,
                suggestions=[],
                overall_analysis=f"MCP service temporarily unavailable: {str(e)[:200]}",
                confidence=0.0,
                no_action_possible=True,
            )

    cache.set(fingerprint, response)
    return response.model_dump()


def _create_backend(backend_name: str):
    """Dynamically create LLM backend instance."""
    if backend_name == "openai":
        from .llm.openai_backend import OpenAIBackend
        return OpenAIBackend({
            "api_key": os.environ.get("OPENAI_API_KEY", ""),
            "model": os.environ.get("AIMV_LLM_MODEL", "gpt-4o"),
        })
    elif backend_name == "anthropic":
        from .llm.anthropic_backend import AnthropicBackend
        return AnthropicBackend({
            "api_key": os.environ.get("ANTHROPIC_API_KEY", ""),
            "model": os.environ.get("AIMV_LLM_MODEL", "claude-sonnet-4-6"),
        })
    elif backend_name == "deepseek":
        from .llm.deepseek_backend import DeepSeekBackend
        return DeepSeekBackend({
            "api_key": os.environ.get("DEEPSEEK_API_KEY", ""),
            "model": os.environ.get("AIMV_LLM_MODEL", "deepseek-v4-pro"),
        })
    return None
