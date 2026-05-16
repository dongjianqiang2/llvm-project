# [AIMV] MCP Server — FastAPI entry point
"""aimv-server — MCP REST API for AI-driven vectorization analysis."""

import os
import time
import logging
from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import JSONResponse
from .middleware import APIKeyMiddleware
from .models import AnalyzeRequest, AnalyzeResponse
from .prompt_builder import build_system_prompt, build_user_prompt
from .suggestion_parser import parse_structured_response, SuggestionParseError
from .cache import DiagnosticCache, compute_diagnostic_fingerprint

logger = logging.getLogger("aimv.mcp")

app = FastAPI(title="AIMV MCP Server", version="0.1.0")
app.add_middleware(APIKeyMiddleware)

# Backend selection via env var (default: mock)
LLM_BACKEND = os.environ.get("AIMV_LLM_BACKEND", "mock")

# Cache
cache = DiagnosticCache(
    ttl_seconds=int(os.environ.get("AIMV_CACHE_TTL", "86400"))
)


@app.get("/api/v1/health")
async def health():
    return {
        "status": "ok",
        "backend": LLM_BACKEND,
        "cache_hits": cache._cache_hits,
        "cache_misses": cache._cache_misses,
    }


@app.get("/api/v1/cache/stats")
async def cache_stats():
    return cache.get_stats()


@app.post("/api/v1/analyze-vectorization")
async def analyze_vectorization(request: Request):
    start = time.monotonic()

    # Parse and validate request
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
        elapsed = (time.monotonic() - start) * 1000
        response = AnalyzeResponse(
            request_id=analyze_req.request_id,
            suggestions=[],
            overall_analysis="Mock backend — no LLM call performed. Response time: "
            f"{elapsed:.0f}ms",
            confidence=0.0,
            no_action_possible=True,
        )
    else:
        try:
            # Real LLM backends loaded dynamically
            if LLM_BACKEND == "openai":
                from .llm.openai_backend import OpenAIBackend
                backend = OpenAIBackend({
                    "api_key": os.environ.get("OPENAI_API_KEY", ""),
                    "model": os.environ.get("AIMV_LLM_MODEL", "gpt-4o"),
                    "base_url": os.environ.get("AIMV_LLM_BASE_URL"),
                })
            elif LLM_BACKEND == "anthropic":
                from .llm.anthropic_backend import AnthropicBackend
                backend = AnthropicBackend({
                    "api_key": os.environ.get("ANTHROPIC_API_KEY", ""),
                    "model": os.environ.get("AIMV_LLM_MODEL", "claude-sonnet-4-6"),
                    "base_url": os.environ.get("AIMV_LLM_BASE_URL"),
                })
            elif LLM_BACKEND == "deepseek":
                from .llm.openai_backend import OpenAIBackend
                backend = OpenAIBackend({
                    "api_key": os.environ.get("DEEPSEEK_API_KEY", ""),
                    "model": os.environ.get("AIMV_LLM_MODEL", "deepseek-v4-pro"),
                    "base_url": os.environ.get("AIMV_LLM_BASE_URL",
                                               "https://api.deepseek.com/v1"),
                })
            else:
                return JSONResponse(
                    status_code=500,
                    content={"detail": f"Unknown LLM backend: {LLM_BACKEND}"},
                )
            response = backend.analyze(analyze_req)
        except SuggestionParseError as e:
            logger.error(f"LLM response parse failed: {e}")
            response = AnalyzeResponse(
                request_id=analyze_req.request_id,
                suggestions=[],
                overall_analysis=f"Failed to parse LLM response: {str(e)[:200]}",
                confidence=0.0,
                no_action_possible=True,
            )
        except Exception as e:
            logger.error(f"LLM call failed: {e}")
            elapsed = (time.monotonic() - start) * 1000
            if elapsed > 60000:  # 60s timeout
                response = AnalyzeResponse(
                    request_id=analyze_req.request_id,
                    suggestions=[],
                    overall_analysis=f"MCP service temporarily unavailable (timeout)",
                    confidence=0.0,
                    no_action_possible=True,
                )
            else:
                return JSONResponse(
                    status_code=500,
                    content={"detail": str(e)},
                )

    # Cache and return
    cache.set(fingerprint, response)
    return response.model_dump()
