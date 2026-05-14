#!/usr/bin/env python3
# [BiSheng] AIMV CI — aimv-run-batch (T6.2)
"""Batch AIMV analysis with per-file serialization, cross-file parallelism."""
import argparse
import asyncio
import json
import sys
import tempfile
from pathlib import Path
from collections import defaultdict


async def run_single(source_file: str, function_name: str, mcp_url: str,
                     aimv_level: str, max_rounds: int, test_cmd: str,
                     output_dir: str) -> dict:
    """Run a single aimv-driver analysis asynchronously."""
    import shutil
    work = Path(tempfile.mkdtemp(prefix="aimv-ci-"))
    src_copy = work / Path(source_file).name
    shutil.copy2(source_file, src_copy)

    cmd = [
        sys.executable, "-m", "aimv.driver.aimv_driver",
        "--function", function_name,
        "--mcp-url", mcp_url,
        "--aimv-level", aimv_level,
        "--max-rounds", str(max_rounds),
        "--test-cmd", test_cmd,
        "--output-dir", f"{output_dir}/{function_name}",
        "--json-log",
        str(src_copy),
    ]

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await proc.communicate()

        sessions_dir = Path(f"{output_dir}/{function_name}/sessions")
        sessions = list(sessions_dir.glob("*.json")) if sessions_dir.exists() else []
        if sessions:
            with open(sessions[0]) as f:
                session_data = json.load(f)
            return {
                "file": source_file, "function": function_name,
                "status": session_data.get("termination_reason", "unknown"),
                "rounds": len(session_data.get("rounds", [])),
                "session_id": session_data.get("session_id"),
            }
        return {
            "file": source_file, "function": function_name,
            "status": "error",
            "error": stderr.decode()[:500] if stderr else "unknown",
            "rounds": 0,
        }
    except Exception as e:
        return {
            "file": source_file, "function": function_name,
            "status": "error", "error": str(e), "rounds": 0,
        }


async def run_batch(changes: list, mcp_url: str, aimv_level: str = "moderate",
                    max_rounds: int = 3, test_cmd: str = "make test",
                    max_parallel: int = 4, output_dir: str = "./aimv-results") -> dict:
    """Batch run AIMV analysis with parallelism control."""
    by_file: dict[str, list] = defaultdict(list)
    for item in changes:
        by_file[item["file"]].append((item["function"], item.get("start_line", 0),
                                       item.get("end_line", 0)))

    semaphore = asyncio.Semaphore(max_parallel)

    async def analyze_file(file: str, funcs: list) -> list:
        results = []
        for func_name, _start, _end in funcs:
            async with semaphore:
                result = await run_single(
                    file, func_name, mcp_url, aimv_level,
                    max_rounds, test_cmd, output_dir,
                )
                results.append(result)
        return results

    tasks = [analyze_file(file, funcs) for file, funcs in by_file.items()]
    all_results = await asyncio.gather(*tasks)

    flat = [r for file_results in all_results for r in file_results]
    return _summarize(flat)


def _summarize(results: list) -> dict:
    total = len(results)
    succeeded = sum(1 for r in results if r.get("status") == "vectorized")
    return {
        "summary": {
            "total_functions": total,
            "vectorized": succeeded,
            "success_rate": succeeded / max(total, 1),
        },
        "details": results,
    }


def main():
    parser = argparse.ArgumentParser(prog="aimv-run-batch")
    parser.add_argument("--input", required=True, help="changes JSON file")
    parser.add_argument("--mcp-url", default="http://localhost:8080")
    parser.add_argument("--aimv-level", default="moderate")
    parser.add_argument("--max-rounds", type=int, default=3)
    parser.add_argument("--test-cmd", default="make test")
    parser.add_argument("--parallel", type=int, default=4)
    parser.add_argument("--output-dir", default="./aimv-results")
    args = parser.parse_args()

    with open(args.input) as f:
        changes = json.load(f)

    result = asyncio.run(run_batch(
        changes, args.mcp_url, args.aimv_level, args.max_rounds,
        args.test_cmd, args.parallel, args.output_dir,
    ))

    json.dump(result, sys.stdout, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
