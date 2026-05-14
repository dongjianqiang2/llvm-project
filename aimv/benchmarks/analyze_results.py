#!/usr/bin/env python3
# [BiSheng] AIMV — Benchmark results analyzer (T4.6)
"""Analyze benchmark timing results and compute speedup vs baseline."""
import json
import sys
import statistics
from pathlib import Path


def load_results(path: str) -> dict:
    with open(path) as f:
        data = json.load(f)
    return {item["benchmark"]: item for item in data}


def analyze(baseline_path: str, optimized_path: str) -> dict:
    baseline = load_results(baseline_path)
    optimized = load_results(optimized_path)

    results = []
    for name, opt in optimized.items():
        base = baseline.get(name)
        if not base:
            results.append({
                "benchmark": name,
                "status": "no_baseline",
                "optimized_median_ms": opt["median_ms"],
            })
            continue

        speedup = ((base["median_ms"] - opt["median_ms"]) / base["median_ms"]) * 100
        results.append({
            "benchmark": name,
            "status": "passed" if speedup >= 5.0 else "insufficient_gain",
            "baseline_median_ms": base["median_ms"],
            "optimized_median_ms": opt["median_ms"],
            "speedup_pct": round(speedup, 2),
        })

    passed = sum(1 for r in results if r["status"] == "passed")
    print(f"Results: {passed}/{len(results)} benchmarks passed (>=5% speedup)")
    for r in results:
        icon = "+" if r["status"] == "passed" else "-"
        speedup = r.get("speedup_pct", "N/A")
        print(f"  {icon} {r['benchmark']}: {speedup}%")
    return {"summary": {"total": len(results), "passed": passed}, "details": results}


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <baseline.json> <optimized.json>")
        sys.exit(1)
    result = analyze(sys.argv[1], sys.argv[2])
    json.dump(result, sys.stdout, indent=2)
