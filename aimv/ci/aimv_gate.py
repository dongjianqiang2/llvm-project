#!/usr/bin/env python3
# [AIMV] AIMV CI — aimv-gate (T6.4)
"""Gate decision logic: report, regression, or enforce modes."""
import argparse
import json
import sys
from pathlib import Path
from typing import Optional


def evaluate_gate(summary: dict, config: dict,
                  baseline: Optional[dict] = None) -> dict:
    """Evaluate gate decision. Returns {"allow": bool, "reason": str}."""
    mode = config.get("mode", "report")

    if mode == "report":
        return {"allow": True, "reason": "report-only mode, always allow"}

    if mode == "regression" and baseline:
        threshold = config.get("regression", {}).get(
            "perf_degradation_threshold_pct", 5.0)
        for item in summary.get("details", []):
            func_name = item["function"]
            baseline_func = _find_baseline(baseline, func_name)
            if baseline_func and baseline_func.get("perf_improvement_pct"):
                delta = (item.get("perf_improvement_pct", 0) -
                         baseline_func["perf_improvement_pct"])
                if delta < -threshold:
                    return {
                        "allow": False,
                        "reason": f"perf regression in {func_name}: {delta:.1f}%",
                    }
        return {"allow": True, "reason": "no regressions detected"}

    if mode == "enforce":
        for rule in config.get("enforce", {}).get("rules", []):
            pattern = rule["path_pattern"]
            min_rate = rule["min_vectorization_rate"]
            matching = [d for d in summary.get("details", [])
                       if _path_matches(d.get("file", ""), pattern)]
            if matching:
                vec_count = sum(1 for d in matching
                               if d.get("status") == "vectorized")
                rate = vec_count / len(matching)
                if rate < min_rate:
                    return {
                        "allow": False,
                        "reason": (
                            f"vectorization rate {rate:.0%} below minimum "
                            f"{min_rate:.0%} for {pattern}"
                        ),
                    }
        return {"allow": True, "reason": "all enforce rules satisfied"}

    return {"allow": True, "reason": "unknown mode, allowing"}


def _find_baseline(baseline: dict, func_name: str) -> Optional[dict]:
    for item in baseline.get("details", []):
        if item.get("function") == func_name:
            return item
    return None


def _path_matches(file_path: str, pattern: str) -> bool:
    from pathlib import PurePosixPath
    p = PurePosixPath(file_path)
    # Try full ** pattern first, then without ** (zero-level match)
    return p.match(pattern) or p.match(pattern.replace("**/", ""))


def main():
    parser = argparse.ArgumentParser(prog="aimv-gate")
    parser.add_argument("--input", required=True, help="Summary JSON file")
    parser.add_argument("--config", help="Gate config YAML")
    parser.add_argument("--baseline", help="Baseline JSON file")
    args = parser.parse_args()

    with open(args.input) as f:
        summary = json.load(f)

    config = {"mode": "report"}
    if args.config:
        import yaml
        with open(args.config) as f:
            config = yaml.safe_load(f).get("gate", {"mode": "report"})

    baseline = None
    if args.baseline:
        with open(args.baseline) as f:
            baseline = json.load(f)

    result = evaluate_gate(summary, config, baseline)
    print(json.dumps(result, indent=2))
    return 0 if result["allow"] else 1


if __name__ == "__main__":
    sys.exit(main())
