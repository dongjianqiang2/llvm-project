# [AIMV] MCP Server — FastAPI entry point
"""aimv-server — MCP REST API for AI-driven vectorization analysis."""

import os
import time
import asyncio
import logging
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
from .middleware import APIKeyMiddleware, AIMVErrorHandler
from .models import AnalyzeRequest, AnalyzeResponse, LoopTransformRequest, LoopTransformResponse, RemarkSeverity
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


@app.post("/api/v1/analyze-loop-transform")
async def analyze_loop_transform(request: Request):
    """T6.1: Analyze loop for transformation opportunities (interchange, fission, etc.)."""
    try:
        body = await request.json()
        transform_req = LoopTransformRequest.model_validate(body)
    except Exception as e:
        return JSONResponse(
            status_code=422,
            content={"detail": [{"msg": str(e)}]},
        )

    # Mock backend: pattern-based transform suggestions
    if LLM_BACKEND == "mock":
        from .llm.mock_backend import MockLLMBackend
        mock = MockLLMBackend()
        # Reuse analyze for the vectorization part, then add transform suggestions
        analyze_req = AnalyzeRequest(
            request_id=transform_req.request_id,
            target=transform_req.target,
            function=transform_req.function,
            diagnostics=transform_req.diagnostics,
            aimv_level=transform_req.aimv_level,
        )
        vec_resp = mock.analyze(analyze_req)

        # Build loop transform suggestions
        from .models import TransformSuggestion
        transform_suggestions = []

        # If nested loop info provided, suggest interchange
        if transform_req.loop_nest:
            ln = transform_req.loop_nest
            if ln.is_row_major is False:
                # Column-major access → interchange helps
                source_file = transform_req.function.source_file or "source.c"
                source_code = transform_req.function.source_code
                lines = source_code.split("\n")
                outer_idx = ln.outer_loop_line - 1
                inner_idx = ln.inner_loop_line - 1
                outer_line = lines[outer_idx] if outer_idx < len(lines) else ""
                inner_line = lines[inner_idx] if inner_idx < len(lines) else ""
                original = outer_line + "\n" + inner_line
                modified = inner_line + "\n" + outer_line
                diff = (
                    f"--- a/{source_file}\n+++ b/{source_file}\n"
                    f"@@ -{ln.outer_loop_line},2 +{ln.outer_loop_line},2 @@\n"
                    f"-{original}\n+{modified}\n"
                )
                transform_suggestions.append(TransformSuggestion(
                    transform_type="interchange",
                    description="Swap outer and inner loops for unit-stride access",
                    reasoning="Column-major memory access causes cache misses. "
                              "Loop interchange makes the inner loop access consecutive memory.",
                    source_file=source_file,
                    line_start=ln.outer_loop_line,
                    line_end=ln.inner_loop_line,
                    original=original,
                    modified=modified,
                    diff=diff,
                    estimated_impact="high",
                    safety_concern="Loop interchange changes memory access order. "
                                   "Verify that no dependencies are violated.",
                ))

        # If vectorization failed, check if fission could help
        for diag in transform_req.diagnostics:
            if diag.severity == RemarkSeverity.MISSED and diag.remark_id == "CantReorderMemOps":
                if not transform_suggestions:
                    source_file = transform_req.function.source_file or "source.c"
                    transform_suggestions.append(TransformSuggestion(
                        transform_type="distribution",
                        description="Distribute loop to isolate vectorizable part",
                        reasoning="Memory dependencies prevent vectorization of the whole loop. "
                                  "Loop distribution (#pragma clang loop distribute(enable)) "
                                  "can isolate the vectorizable portion.",
                        source_file=source_file,
                        line_start=transform_req.function.loop_line,
                        line_end=transform_req.function.loop_line,
                        original="",
                        modified="#pragma clang loop distribute(enable)",
                        diff=f"--- a/{source_file}\n+++ b/{source_file}\n"
                             f"@@ -{transform_req.function.loop_line},1 +{transform_req.function.loop_line},1 @@\n"
                             f"+#pragma clang loop distribute(enable)\n",
                        estimated_impact="medium",
                        safety_concern="Distribution requires no cross-iteration dependencies "
                                       "between the distributed parts.",
                    ))
                break

        if not transform_suggestions and vec_resp.no_action_possible:
            return LoopTransformResponse(
                request_id=transform_req.request_id,
                suggestions=[],
                overall_analysis="No loop transformation opportunities identified.",
                confidence=1.0,
                no_action_possible=True,
            ).model_dump()

        overall = "Mock analysis: " + "; ".join(
            s.description for s in transform_suggestions
        ) if transform_suggestions else vec_resp.overall_analysis

        return LoopTransformResponse(
            request_id=transform_req.request_id,
            suggestions=transform_suggestions,
            overall_analysis=overall,
            confidence=0.8,
            no_action_possible=len(transform_suggestions) == 0,
        ).model_dump()

    # Non-mock backend: delegate to LLM with loop transform template
    return LoopTransformResponse(
        request_id=transform_req.request_id,
        suggestions=[],
        overall_analysis="Loop transform analysis requires LLM backend (not yet connected).",
        confidence=0.0,
        no_action_possible=True,
    ).model_dump()


def _create_backend(backend_name: str):
    """Dynamically create LLM backend instance.

    OpenAI backend env vars:
      OPENAI_API_KEY   — API key (required)
      OPENAI_BASE_URL  — custom base URL (optional, e.g. https://api.openai.com/v1)
      OPENAI_MODEL     — model name (optional, falls back to AIMV_LLM_MODEL then gpt-4o)

    Anthropic backend env vars:
      ANTHROPIC_API_KEY  — API key (required)
      ANTHROPIC_BASE_URL — custom base URL (optional, e.g. https://open.bigmodel.cn/api/anthropic)
      ANTHROPIC_MODEL    — model name (optional, falls back to AIMV_LLM_MODEL then claude-sonnet-4-6)
    """
    if backend_name == "openai":
        from .llm.openai_backend import OpenAIBackend
        return OpenAIBackend({
            "api_key": os.environ.get("OPENAI_API_KEY", ""),
            "base_url": os.environ.get("OPENAI_BASE_URL", ""),
            "model": os.environ.get("OPENAI_MODEL",
                       os.environ.get("AIMV_LLM_MODEL", "gpt-4o")),
        })
    elif backend_name == "anthropic":
        from .llm.anthropic_backend import AnthropicBackend
        return AnthropicBackend({
            "api_key": os.environ.get("ANTHROPIC_API_KEY", ""),
            "base_url": os.environ.get("ANTHROPIC_BASE_URL", ""),
            "model": os.environ.get("ANTHROPIC_MODEL",
                       os.environ.get("AIMV_LLM_MODEL", "claude-sonnet-4-6")),
        })
    return None
