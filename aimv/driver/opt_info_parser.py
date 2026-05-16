# [AIMV] AIMV Driver — opt-record YAML/JSON parser (YAML mode support)
import yaml
import json
from pathlib import Path
from typing import Optional


def parse_to_analyze_request(
    opt_record_path: str, function_name: str, source_file: str,
) -> dict:
    """Parse opt-record YAML/JSON into AnalyzeRequest-compatible dict.

    YAML mode produces basic diagnostics (no cost_model, no dependencies).
    """
    with open(opt_record_path) as f:
        content = f.read()

    if opt_record_path.endswith(".json"):
        records = json.loads(content)
    else:
        records = yaml.safe_load(content) or []

    diagnostics = []
    for record in records:
        if not isinstance(record, dict):
            continue
        func = record.get("Function", "")
        pas = str(record.get("Pass", ""))
        if func != function_name or "loop-vectorize" not in pas:
            continue

        sev = "missed"
        t = record.get("type", "")
        if "passed" in str(t).lower():
            sev = "passed"
        elif "analysis" in str(t).lower():
            sev = "analysis"

        diagnostics.append({
            "pass_name": pas,
            "remark_id": record.get("Name", ""),
            "remark_text": record.get("Name", ""),
            "severity": sev,
            "function_name": function_name,
            "loop_location": record.get("DebugLoc", ""),
            "source_context": "",
            "ir_snippet": "",
            "cost_model": None,
            "dependencies": [],
            "memory_info": None,
            "loop_info": None,
        })

    return {
        "request_id": f"aimv-yaml-{function_name}",
        "target": _detect_target(source_file),
        "diagnostics": diagnostics,
    }


def _detect_target(source_file: str) -> dict:
    return {
        "triple": "armv7-unknown-linux-gnueabi",
        "cpu": "cortex-a9",
        "features": ["neon"],
        "vector_width": 128,
    }
