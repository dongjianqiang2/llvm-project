#!/usr/bin/env python3
# [BiSheng] AIMV Driver — CLI + main iteration loop
"""aimv-driver — AI-driven loop vectorization optimization orchestration."""

import argparse
import json
import re
import shutil
import sys
import time
from pathlib import Path
from typing import Optional

from .config import load_config
from .build_orchestrator import BuildOrchestrator
from .source_manager import SourceManager
from .mcp_client import MCPClient
from .iteration_engine import IterationEngine, NextAction
from .session_store import (
    SessionStore, SessionRecord, RoundRecord, DriverStatus, TerminationReason,
)
from . import opt_info_parser


# ---------------------------------------------------------------------------
# Source extraction helpers (T3.8)
# ---------------------------------------------------------------------------

def extract_function_source(source_file: str, func_name: str) -> Optional[str]:
    """Extract source code of a named function from a C file.

    Uses regex to find the function body. Falls back to None if not found.
    """
    text = Path(source_file).read_text()
    # Regex: match return type + function name + params + opening brace
    # Captures from the start of the line (or type qualifiers) through the opening {
    pattern = re.compile(
        r'(?:^|\n)\s*((?:static\s+|inline\s+|extern\s+)*[\w\s*]+'
        + re.escape(func_name)
        + r'\s*\([^)]*\)\s*\{)',
        re.MULTILINE,
    )
    m = pattern.search(text)
    if not m:
        return None
    start = m.start() + 1 if m.start() > 0 else 0  # skip the leading \n
    brace_start = m.end() - 1  # position of {
    depth = 0
    i = brace_start
    while i < len(text):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    return None


def extract_function_signature(source_file: str, func_name: str) -> str:
    """Extract function signature from a C file."""
    text = Path(source_file).read_text()
    pattern = re.compile(
        r'((?:static\s+|inline\s+|extern\s+)*[\w\s*]+'
        + re.escape(func_name)
        + r'\s*\([^)]*\))',
        re.MULTILINE,
    )
    m = pattern.search(text)
    return m.group(1).strip() if m else func_name


def extract_loop_line(diagnostics: list, func_name: str) -> int:
    """Extract loop source line from diagnostics."""
    for d in diagnostics:
        if d.get("function_name") == func_name:
            loc = d.get("loop_location", "")
            m = re.search(r':(\d+)', loc)
            if m:
                return int(m.group(1))
    return 0


def extract_lines_around(source_file: str, line: int, context: int = 40) -> str:
    """Extract lines around a given line number from a file."""
    if line <= 0:
        return Path(source_file).read_text()
    lines = Path(source_file).read_text().split('\n')
    start = max(0, line - context - 1)
    end = min(len(lines), line + context)
    return '\n'.join(lines[start:end])


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _check_target_loop_passed(vstatus, target_loop: Optional[str]) -> bool:
    if target_loop is None:
        return False
    return not any(
        target_loop in d.get("loop_location", "")
        for d in vstatus.missed_details
    )


def build_history(session: SessionRecord) -> list:
    """Build history list for MCP request from previous rounds."""
    history = []
    for r in session.rounds:
        if r.patch:
            history.append({
                "round": r.round_number,
                "suggestion_description": "patch applied",
                "applied_diff": r.patch.diff_text,
                "result": (
                    "compiled and tested OK"
                    if (r.compile_success and r.test_success) else "failed"
                ),
            })
    return history


# ---------------------------------------------------------------------------
# Main loop (T3.9)
# ---------------------------------------------------------------------------

def main_loop(driver_config: dict) -> int:
    """Main AIMV iteration loop. Returns 0 on success, non-zero on failure."""

    func_name = driver_config.get("function")
    source_file = driver_config.get("source_file")
    if not func_name or not source_file:
        print("Error: --function and source_file are required", file=sys.stderr)
        return 2

    session = SessionRecord(
        function_name=func_name,
        source_files=[source_file],
        aimv_level=driver_config.get("aimv_level", "moderate"),
        max_rounds=driver_config.get("max_rounds", 5),
        cli_command=" ".join(sys.argv),
    )

    builder = BuildOrchestrator(driver_config)
    sources = SourceManager(driver_config.get("backup_dir",
                             f"{driver_config.get('output_dir', './aimv-output')}/backups"))
    mcp = MCPClient(driver_config.get("mcp_url", "http://localhost:8080"),
                    driver_config.get("mcp_timeout_seconds", 60))
    engine = IterationEngine(session.aimv_level, session.max_rounds)
    store = SessionStore(driver_config.get("output_dir", "./aimv-output"))

    # Pristine backup
    pristine_dir = Path(driver_config.get("backup_dir",
                        f"{driver_config.get('output_dir', './aimv-output')}/backups")) / "pristine"
    pristine_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_file, pristine_dir / Path(source_file).name)

    try:
        while True:
            round_rec = RoundRecord(round_number=len(session.rounds) + 1)
            session.rounds.append(round_rec)
            session.current_round = round_rec.round_number

            # Step 1: Compile + AIMV Pass
            round_rec.status = DriverStatus.COMPILING
            build = builder.compile_with_aimv(
                source_file=source_file,
                output_file=driver_config.get("output_binary",
                               f"{session.function_name}.o"),
                target_function=func_name,
            )
            round_rec.compile_success = (build.returncode == 0)

            if build.returncode != 0:
                action, reason = engine.decide(
                    round_rec.round_number, False, True,
                    False, False, True,
                )
                if action == NextAction.STOP:
                    session.termination_reason = TerminationReason.COMPILE_ERROR
                    break
                sources.rollback_all()
                continue

            # Check vectorization status
            if Path(build.aimv_json_path).exists():
                vstatus = builder.check_vectorization_from_json(
                    build.aimv_json_path, func_name)
            else:
                vstatus = builder.check_vectorization_from_yaml(
                    build.opt_record_path, func_name)
            round_rec.vectorization_status = vstatus

            # Auto-select target loop on first round
            if session.target_loop_line is None and vstatus.missed_details:
                session.target_loop_line = vstatus.missed_details[0].get(
                    "loop_location")

            # Termination check
            if vstatus.total_loops > 0:
                if _check_target_loop_passed(vstatus, session.target_loop_line):
                    session.termination_reason = TerminationReason.VECTORIZED
                    break
                elif vstatus.missed_loops == 0:
                    session.termination_reason = TerminationReason.VECTORIZED
                    break
            elif vstatus.total_loops == 0:
                session.termination_reason = TerminationReason.NO_SUGGESTION
                break

            # Load diagnostics
            if Path(build.aimv_json_path).exists():
                with open(build.aimv_json_path) as f:
                    aimv_json = json.load(f)
            else:
                aimv_json = opt_info_parser.parse_to_analyze_request(
                    build.opt_record_path, func_name, source_file)

            # Build function field
            func_source = extract_function_source(source_file, func_name)
            if func_source is None:
                loop_line = extract_loop_line(
                    aimv_json.get("diagnostics", []), func_name)
                func_source = extract_lines_around(source_file, loop_line)

            aimv_json["function"] = {
                "name": func_name,
                "signature": extract_function_signature(source_file, func_name),
                "source_code": func_source,
                "source_file": source_file,
                "loop_line": extract_loop_line(
                    aimv_json.get("diagnostics", []), func_name),
            }
            aimv_json["history"] = build_history(session)
            aimv_json["aimv_level"] = engine.current_level

            # Step 2: MCP query
            round_rec.status = DriverStatus.QUERYING_MCP
            mcp_resp = mcp.analyze(aimv_json)
            round_rec.mcp_response = mcp_resp

            mcp_responded = mcp_resp is not None
            mcp_had_suggestions = bool(mcp_resp and mcp_resp.get("suggestions"))

            if not mcp_responded or not mcp_had_suggestions:
                action, reason = engine.decide(
                    round_rec.round_number, True, True,
                    False, mcp_had_suggestions, mcp_responded,
                )
                if action == NextAction.ESCALATE_LEVEL:
                    continue
                if action == NextAction.STOP:
                    session.termination_reason = TerminationReason.NO_SUGGESTION
                    break
                continue

            # Step 3: Apply patch
            round_rec.status = DriverStatus.PATCHING
            suggestion = mcp_resp["suggestions"][0]

            new_diff = suggestion["diff"]
            if any(p.diff_text.strip() == new_diff.strip()
                   for p in sources._patch_history):
                session.termination_reason = TerminationReason.NO_IMPROVEMENT
                store.save(session)
                break

            patch = sources.apply_patch(suggestion["source_file"], new_diff)
            round_rec.patch = patch
            store.save(session)

            # Step 4: Verify compilation
            round_rec.status = DriverStatus.VERIFYING
            verify_build = builder.compile_with_aimv(
                source_file=source_file,
                output_file=driver_config.get("output_binary"),
                target_function=func_name,
            )
            store.save(session)

            if verify_build.returncode != 0:
                sources.rollback(patch)
                store.save(session)
                action, reason = engine.decide(
                    round_rec.round_number, False, True,
                    False, True, True,
                )
                if action == NextAction.STOP:
                    session.termination_reason = TerminationReason.COMPILE_ERROR
                    break
                continue

            # Step 5: Tests
            if driver_config.get("test_cmd"):
                test = builder.run_tests(driver_config["test_cmd"])
                round_rec.test_success = (test.returncode == 0 and test.failed == 0)
                if test.returncode != 0 or test.failed > 0:
                    sources.rollback(patch)
                    round_rec.status = DriverStatus.FAILED
                    session.termination_reason = TerminationReason.TEST_FAILURE
                    store.save(session)
                    break

            # Step 6: Optional perf measurement
            if driver_config.get("measure_perf"):
                round_rec.status = DriverStatus.MEASURING
                # baseline from first round
                baseline = session.rounds[0].baseline_perf_ms
                current = baseline  # placeholder — real measurement via perf_cmd
                perf_delta = ((baseline - current) / baseline * 100) if baseline else None

                action, reason = engine.decide(
                    round_rec.round_number, True, True,
                    False, True, True, perf_delta,
                )
                if action == NextAction.ROLLBACK:
                    sources.rollback(patch)
                    session.termination_reason = TerminationReason.NO_IMPROVEMENT
                    break
            else:
                action, reason = engine.decide(
                    round_rec.round_number, True, True,
                    False, True, True,
                )

            if action == NextAction.STOP:
                session.termination_reason = TerminationReason.ROUND_LIMIT
                break

            store.save(session)

    except KeyboardInterrupt:
        session.termination_reason = TerminationReason.INTERRUPTED
        sources.rollback_all()

    finally:
        session.finished_at = time.time()
        session.total_elapsed_ms = (session.finished_at - session.started_at) * 1000
        store.save(session)

    return 0 if session.termination_reason == TerminationReason.VECTORIZED else 1


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def build_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="aimv-driver")
    parser.add_argument("source_file", help="C/C++ source file")
    parser.add_argument("--function", dest="function", help="Target function name")
    parser.add_argument("--aimv-level", default="moderate",
                        choices=["conservative", "moderate", "aggressive"])
    parser.add_argument("--max-rounds", type=int, default=5)
    parser.add_argument("--mcp-url", default="http://localhost:8080")
    parser.add_argument("--output-dir", default="./aimv-output")
    parser.add_argument("--build-cmd", dest="build_cmd")
    parser.add_argument("--test-cmd", dest="test_cmd")
    parser.add_argument("--perf-cmd", dest="perf_cmd")
    parser.add_argument("--measure-perf", action="store_true")
    parser.add_argument("--resume", dest="resume")
    parser.add_argument("--list-sessions", action="store_true")
    parser.add_argument("--mode", choices=["pass", "yaml"], default="pass")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--json-log", action="store_true")
    parser.add_argument("--loop-line", type=int)
    return parser


def main(argv=None):
    parser = build_argparser()
    args = parser.parse_args(argv)

    if args.verbose:
        print(f"[aimv-driver] args: {args}", file=sys.stderr)

    cfg = load_config()
    cfg["function"] = args.function
    cfg["source_file"] = args.source_file
    cfg["aimv_level"] = args.aimv_level
    cfg["max_rounds"] = args.max_rounds
    cfg["mcp_url"] = args.mcp_url
    cfg["output_dir"] = args.output_dir
    if args.test_cmd:
        cfg["test_cmd"] = args.test_cmd
    if args.measure_perf:
        cfg["measure_perf"] = True
    if args.dry_run:
        cfg["dry_run"] = True

    return main_loop(cfg)


if __name__ == "__main__":
    sys.exit(main())
