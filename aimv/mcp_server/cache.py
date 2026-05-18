# [AIMV] MCP Server — Diagnostic fingerprint cache
import hashlib
import json
import threading
import time
from typing import Optional
from .models import AnalyzeRequest, AnalyzeResponse


def compute_diagnostic_fingerprint(request: AnalyzeRequest, mode: str = "strict") -> str:
    """Compute stable fingerprint for cache matching.

    Strict mode: includes source_code hash + history (same function + same history → cache hit)
    Relaxed mode: only target + diagnostics pattern (cross-function similar patterns → cache hit)
    """
    if mode == "relaxed":
        canonical = {
            "target_triple": request.target.triple,
            "target_cpu": request.target.cpu,
            "target_features": sorted(request.target.features),
            "vector_width": request.target.vector_width,
            "level": request.aimv_level.value,
            "diagnostics": [
                {
                    "pass": diag.pass_name,
                    "remark_id": diag.remark_id,
                    "remark_prefix": diag.remark_text[:120],
                }
                for diag in request.diagnostics
            ],
        }
    else:
        # Strict: includes source_code hash + history
        canonical = {
            "target_triple": request.target.triple,
            "target_cpu": request.target.cpu,
            "target_features": sorted(request.target.features),
            "vector_width": request.target.vector_width,
            "level": request.aimv_level.value,
            "function_name": request.function.name,
            "source_code_sha256": hashlib.sha256(
                request.function.source_code.encode()
            ).hexdigest(),
            "diagnostics": [
                {
                    "pass": diag.pass_name,
                    "remark_id": diag.remark_id,
                    "remark_prefix": diag.remark_text[:120],
                }
                for diag in request.diagnostics
            ],
            "history": [
                {
                    "round": h.round,
                    "diagnosis_summary": h.diagnosis_summary,
                    "suggestion_applied": h.suggestion_applied,
                    "outcome": h.outcome,
                }
                for h in request.history
            ] if request.history else [],
        }

    payload = json.dumps(canonical, sort_keys=True).encode()
    prefix = "relaxed" if mode == "relaxed" else "strict"
    return f"{prefix}:{hashlib.sha256(payload).hexdigest()[:16]}"


class DiagnosticCache:
    """Two-tier diagnostic pattern cache (L1 memory + optional L2 Redis).

    Same diagnostic fingerprint → cached AI suggestion.
    """

    def __init__(self, redis_client=None, ttl_seconds: int = 86400):
        self._ttl = ttl_seconds
        self._redis = redis_client
        self._local: dict = {}  # fingerprint → (expiry, AnalyzeResponse)
        self._lock = threading.Lock()
        self._total_requests = 0
        self._cache_hits = 0
        self._cache_misses = 0

    def get(self, fingerprint: str) -> Optional[AnalyzeResponse]:
        self._total_requests += 1
        # L1: local memory
        with self._lock:
            entry = self._local.get(fingerprint)
            if entry and entry[0] > time.time():
                self._cache_hits += 1
                return entry[1]

        # L2: Redis
        if self._redis:
            raw = self._redis.get(f"aimv:cache:{fingerprint}")
            if raw:
                data = json.loads(raw)
                resp = AnalyzeResponse(**data)
                with self._lock:
                    self._local[fingerprint] = (time.time() + self._ttl, resp)
                self._cache_hits += 1
                return resp

        self._cache_misses += 1
        return None

    def set(self, fingerprint: str, response: AnalyzeResponse):
        expiry = time.time() + self._ttl

        with self._lock:
            self._local[fingerprint] = (expiry, response)
            if len(self._local) > 10000:
                oldest = min(self._local, key=lambda k: self._local[k][0])
                del self._local[oldest]

        if self._redis:
            self._redis.setex(
                f"aimv:cache:{fingerprint}",
                self._ttl,
                response.model_dump_json(),
            )

    def get_stats(self) -> dict:
        return {
            "total_entries": len(self._local),
            "total_requests": self._total_requests,
            "cache_hits": self._cache_hits,
            "cache_misses": self._cache_misses,
            "hit_rate": (self._cache_hits / max(self._total_requests, 1)),
            "estimated_cost_saved_usd": self._cache_hits * 0.01,
        }
