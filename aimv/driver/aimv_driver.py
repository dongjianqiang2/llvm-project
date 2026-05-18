#!/usr/bin/env python3
# [AIMV] AIMV Driver — CLI + main iteration loop
"""aimv-driver — AI-driven loop vectorization optimization orchestration."""

import argparse
import hashlib
import json
import re
import shutil
import sys
import time
import uuid
from pathlib import Path
from typing import Optional

from .config import load_config, DriverConfig
from .build_orchestrator import BuildOrchestrator
from .source_manager import SourceManager
from .mcp_client import MCPClient
from .iteration_engine import IterationEngine
from .session_store import SessionStore
from .models import (
    NextAction, TerminationReason, IterationStatus,
    SessionRecord, PerFunctionResult, RoundRecord,
)
from . import opt_info_parser


# ---------------------------------------------------------------------------
# Source extraction helpers
# ---------------------------------------------------------------------------

def extract_function_source(source_file: str, func_name: str) -> Optional[str]:
    """Extract source code of a named function from a C file."""
    text = Path(source_file).read_text()
    pattern = re.compile(
        r'(?:^|\n)\s*((?:static\s+|inline\s+|extern\s+)*[\w\s*]+'
        + re.escape(func_name)
        + r'\s*\([^)]*\)\s*\{)',
        re.MULTILINE,
    )
    m = pattern.search(text)
    if not m:
        return None
    start = m.start() + 1 if m.start() > 0 else 0
    brace_start = m.end() - 1
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
            m = re.search(r":(\d+):\d+$", loc)
            if m:
                return int(m.group(1))
    return 1


# ---------------------------------------------------------------------------
# MCP request builder (T3.6)
# ---------------------------------------------------------------------------

def build_mcp_request(
    function_name: str,
    source_file: str,
    diagnostics: list,
    target: dict,
    history: list,
    aimv_level: str,
    config: DriverConfig,
    session_id: str = "",
    round_num: int = 1,
) -> dict:
    """Construct MCP AnalyzeRequest (MCP_DESIGN.md §2.1).

    request_id: "aimv-<session_id>-<func_hash[:8]>-r<round>"
    """
    func_hash = hashlib.sha256(function_name.encode()).hexdigest()[:8]

    with open(source_file, "r", encoding="utf-8") as f:
        source_code = f.read()

    loop_line = 1
    for d in diagnostics:
        if d.get("function_name") == function_name and d.get("severity") == "missed":
            loc = d.get("loop_location", "")
            m = re.search(r":(\d+):\d+$", loc)
            if m:
                try:
                    loop_line = int(m.group(1))
                except ValueError:
                    pass
            break

    return {
        "request_id": f"aimv-{session_id}-{func_hash}-r{round_num}",
        "target": target or {
            "triple": "",
            "cpu": "",
            "features": [],
            "vector_width": 128,
        },
        "function": {
            "name": function_name,
            "signature": "",
            "source_code": source_code,
            "source_file": source_file,
            "loop_line": loop_line,
        },
        "diagnostics": [
            d for d in diagnostics
            if d.get("function_name") == function_name and d.get("severity") == "missed"
        ],
        "history": history,
        "aimv_level": aimv_level,
    }


# ---------------------------------------------------------------------------
# History builder (DRIVER_DESIGN §9)
# ---------------------------------------------------------------------------

def build_history(func_result: PerFunctionResult, max_entries: int = 3) -> list:
    """Build history list from PerFunctionResult (last 3 rounds)."""
    rounds = func_result.rounds[-max_entries:]
    history = []
    for r in rounds:
        if r.finished_at is None:
            continue
        history.append({
            "round": r.round_number,
            "diagnosis_summary": _summarize_diagnostics(r.diagnostics_json),
            "suggestion_applied": r.applied_diff_summary or "N/A",
            "outcome": _classify_outcome(r),
        })
    return history


def _summarize_diagnostics(diagnostics_json: Optional[dict]) -> str:
    if not diagnostics_json:
        return "N/A"
    diags = diagnostics_json.get("diagnostics", [])
    if not diags:
        return "N/A"
    first = diags[0]
    return f"{first.get('remark_id', 'unknown')}: {first.get('remark_text', '')[:100]}"


def _classify_outcome(round_rec: RoundRecord) -> str:
    if round_rec.vectorization_status and round_rec.vectorization_status.missed_loops == 0:
        return "vectorized"
    if round_rec.verify_build and round_rec.verify_build.returncode != 0:
        return "compile_failed"
    if round_rec.test_result and round_rec.test_result.failed > 0:
        return "test_failed"
    return "compile_passed, vectorization_still_failed"


# ---------------------------------------------------------------------------
# Review mode (DRIVER_DESIGN §10)
# ---------------------------------------------------------------------------

def prompt_review(diff_text: str, description: str) -> str:
    """AIMV_MODE=review: pause for user confirmation before applying patch."""
    print(f"\n[AIMV] Suggested change: {description}", file=sys.stderr)
    print(f"[AIMV] Diff:\n{diff_text}", file=sys.stderr)
    print("[AIMV] Apply this change? [y/N/r/q]: ", end="", file=sys.stderr, flush=True)
    try:
        response = input().strip().lower()
    except EOFError:
        return "n"
    return response if response in ("y", "r", "q") else "n"


# ---------------------------------------------------------------------------
# Cross-function regression check
# ---------------------------------------------------------------------------

def _check_cross_function_regression(
    source_file: str,
    vectorized_results: list,
    builder: BuildOrchestrator,
    config: DriverConfig,
) -> bool:
    """Check if recent patches broke previously vectorized functions."""
    build = builder.compile_with_aimv(
        source_file=source_file,
        output_file=str(Path(config.output_dir) / "regression_check.o"),
    )
    if build.returncode != 0:
        return True

    for result in vectorized_results:
        vstatus = builder.check_vectorization(build.aimv_json_path, result.function_name)
        if vstatus.missed_loops > 0:
            return True
    return False


# ---------------------------------------------------------------------------
# Single function iteration loop (T3.7)
# ---------------------------------------------------------------------------

def process_single_function(
    function_name: str,
    source_file: str,
    initial_diagnostics: list,
    config: DriverConfig,
    builder: BuildOrchestrator,
    mcp: MCPClient,
    engine: IterationEngine,
    sources: SourceManager,
    store: SessionStore,
    session: SessionRecord,
) -> PerFunctionResult:
    """Single function iteration loop (DRIVER_DESIGN §11)."""
    func_result = PerFunctionResult(function_name=function_name)

    sources.warn_stale_shadow(source_file)

    prev_passed_count = 0

    try:
        while True:
            round_num = len(func_result.rounds) + 1
            round_rec = RoundRecord(round_number=round_num)
            func_result.rounds.append(round_rec)

            # ── Step 1: Compile + AIMV Pass ──
            round_rec.status = IterationStatus.COMPILING
            build = builder.compile_with_aimv(
                source_file=source_file,
                output_file=str(Path(config.output_dir) / f"{function_name}.o"),
                aimv_json_output=str(
                    Path(config.output_dir) / f"aimv-{function_name}-r{round_num}.json"
                ),
            )
            round_rec.build_result = build

            if build.returncode != 0:
                action, reason = engine.decide(
                    current_round=round_num,
                    build_result_ok=False, test_result_ok=True,
                    vectorized=False, mcp_had_suggestions=False,
                    mcp_responded=True,
                    compile_phase="source",
                )
                if action == NextAction.STOP:
                    func_result.termination_reason = TerminationReason.COMPILE_ERROR
                    break
                continue

            # Check vectorization status
            vstatus = builder.check_vectorization(build.aimv_json_path, function_name)
            round_rec.vectorization_status = vstatus

            # Termination: vectorized
            if vstatus.missed_loops == 0 and vstatus.total_loops > 0:
                func_result.termination_reason = TerminationReason.VECTORIZED
                func_result.vectorized = True
                func_result.rounds_used = round_num
                round_rec.status = IterationStatus.SUCCESS
                break

            # Load diagnostics
            with open(build.aimv_json_path) as f:
                aimv_json = json.load(f)
            round_rec.diagnostics_json = aimv_json

            # Build MCP request
            request_body = build_mcp_request(
                function_name=function_name,
                source_file=source_file,
                diagnostics=aimv_json.get("diagnostics", []),
                target=aimv_json.get("target", {}),
                history=build_history(func_result),
                aimv_level=engine.current_level,
                config=config,
                session_id=session.session_id,
                round_num=round_num,
            )
            round_rec.mcp_request = request_body

            # ── Step 2: MCP query [no lock] ──
            round_rec.status = IterationStatus.QUERYING

            # Record pre-query hash for race detection
            pre_query_hash = sources.snapshot_hash(source_file)

            mcp_resp = mcp.analyze(request_body)
            round_rec.mcp_response = mcp_resp
            round_rec.suggestion_description = (
                mcp_resp.get("suggestions", [{}])[0].get("description")
                if mcp_resp and mcp_resp.get("suggestions") else None
            )

            mcp_responded = mcp_resp is not None
            mcp_had_suggestions = bool(
                mcp_resp and mcp_resp.get("suggestions")
                and not mcp_resp.get("no_action_possible")
            )

            if not mcp_responded or not mcp_had_suggestions:
                action, reason = engine.decide(
                    current_round=round_num,
                    build_result_ok=True, test_result_ok=True,
                    vectorized=False, mcp_had_suggestions=mcp_had_suggestions,
                    mcp_responded=mcp_responded,
                )
                if action == NextAction.ESCALATE_LEVEL:
                    continue
                if action == NextAction.STOP:
                    func_result.termination_reason = TerminationReason.NO_SUGGESTION
                    break
                continue

            # Review mode check
            suggestion = mcp_resp["suggestions"][0]
            diff_text = suggestion["diff"]
            round_rec.applied_diff_summary = suggestion.get("description", "")

            if config.aimv_mode == "review" or engine._review_mode:
                response = prompt_review(diff_text, suggestion.get("description", ""))
                if response == "n":
                    continue
                elif response == "r":
                    sources.rollback_all()
                    func_result.termination_reason = TerminationReason.INTERRUPTED
                    break
                elif response == "q":
                    func_result.termination_reason = TerminationReason.INTERRUPTED
                    break

            # Loop detection: prevent LLM from repeating same diff
            if sources.has_diff(diff_text):
                func_result.termination_reason = TerminationReason.NO_IMPROVEMENT
                break

            # ── Step 3: Shadow file patch [acquire lock] ──
            round_rec.status = IterationStatus.PATCHING
            if not sources.acquire_lock(source_file):
                func_result.termination_reason = TerminationReason.LOCK_TIMEOUT
                break

            try:
                # Race detection: check if file changed during MCP query
                if sources.check_stale(source_file, pre_query_hash):
                    # File modified during lock-free MCP query, discard suggestion
                    sources.release_lock(source_file)
                    continue

                patch = sources.apply_shadow_patch(source_file, diff_text)
                round_rec.patch = patch

                # ── Step 4: Compile verify [same lock region] ──
                round_rec.status = IterationStatus.VERIFYING
                shadow_file = source_file + ".aimv-tmp"
                verify_json = str(
                    Path(config.output_dir) / f"aimv-{function_name}-verify-r{round_num}.json"
                )
                verify_build = builder.compile_with_aimv(
                    source_file=shadow_file,
                    output_file=str(
                        Path(config.output_dir) / f"{function_name}-verify.o"
                    ),
                    aimv_json_output=verify_json,
                )
                round_rec.verify_build = verify_build

                if verify_build.returncode != 0:
                    sources.discard_shadow(source_file)
                    action, reason = engine.decide(
                        current_round=round_num,
                        build_result_ok=False, test_result_ok=True,
                        vectorized=False, mcp_had_suggestions=True,
                        mcp_responded=True,
                        compile_phase="patch",
                    )
                    if action == NextAction.STOP:
                        func_result.termination_reason = TerminationReason.COMPILE_ERROR
                        break
                    continue

                # Check vectorization after patch
                verify_vstatus = builder.check_vectorization(
                    verify_build.aimv_json_path, function_name)

                # Regression: passed remark count decreased
                if (prev_passed_count > 0 and
                        verify_vstatus.passed_remark_count < prev_passed_count):
                    sources.discard_shadow(source_file)
                    action, reason = engine.decide(
                        current_round=round_num,
                        build_result_ok=True, test_result_ok=True,
                        vectorized=False, mcp_had_suggestions=True,
                        mcp_responded=True,
                        passed_remark_delta=(
                            verify_vstatus.passed_remark_count - prev_passed_count),
                    )
                    func_result.termination_reason = TerminationReason.NO_IMPROVEMENT
                    break

                # Tests
                if config.test_cmd:
                    test = builder.run_tests(config.test_cmd)
                    round_rec.test_result = test

                    if test.returncode != 0 or test.failed > 0:
                        sources.discard_shadow(source_file)
                        func_result.termination_reason = TerminationReason.TEST_FAILURE
                        break

                # All passed → atomic mv replace
                sources.commit_shadow(source_file)
                prev_passed_count = verify_vstatus.passed_remark_count

            finally:
                sources.release_lock(source_file)

            # Persist session (outside lock)
            store.save(session)

            # Round decision
            action, reason = engine.decide(
                current_round=round_num,
                build_result_ok=True, test_result_ok=True,
                vectorized=False, mcp_had_suggestions=True,
                mcp_responded=True,
            )
            if action == NextAction.STOP:
                func_result.termination_reason = TerminationReason.ROUND_LIMIT
                func_result.rounds_used = round_num
                break

    except KeyboardInterrupt:
        func_result.termination_reason = TerminationReason.INTERRUPTED
        sources.rollback_all()

    finally:
        func_result.rounds_used = len(func_result.rounds)
        if func_result.rounds:
            func_result.rounds[-1].finished_at = time.time()

    return func_result


# ---------------------------------------------------------------------------
# --from-json entry point (T3.8)
# ---------------------------------------------------------------------------

def main_from_json(aimv_json_path: str, source_file: str) -> int:
    """--from-json entry: called by clang Driver fork+exec."""
    with open(aimv_json_path) as f:
        aimv_data = json.load(f)

    diagnostics = aimv_data.get("diagnostics", [])

    failed_functions = list(dict.fromkeys(
        d["function_name"]
        for d in diagnostics
        if d.get("severity") == "missed"
    ))

    if not failed_functions:
        print("[AIMV] all loops already vectorized, nothing to do", file=sys.stderr)
        return 0

    config = load_config()

    builder = BuildOrchestrator(config)
    mcp = MCPClient(config.mcp_url, config.mcp_timeout, api_key=config.mcp_api_key)
    sources = SourceManager(config.output_dir)
    store = SessionStore(config.output_dir)

    pristine_dir = Path(config.output_dir) / "backups" / "pristine"
    pristine_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_file, pristine_dir / Path(source_file).name)

    session = SessionRecord(
        source_file=source_file,
        aimv_level=config.aimv_level,
        max_rounds=config.max_rounds,
    )

    results: list[PerFunctionResult] = []

    for func_name in failed_functions:
        engine = IterationEngine(config.aimv_level, config.max_rounds)
        func_result = process_single_function(
            function_name=func_name,
            source_file=source_file,
            initial_diagnostics=[
                d for d in diagnostics
                if d["function_name"] == func_name and d.get("severity") == "missed"
            ],
            config=config,
            builder=builder,
            mcp=mcp,
            engine=engine,
            sources=sources,
            store=store,
            session=session,
        )
        results.append(func_result)
        session.functions.append(func_result)

        # Cross-function regression check
        if func_result.termination_reason == TerminationReason.VECTORIZED:
            regression = _check_cross_function_regression(
                source_file,
                [r for r in results if r.termination_reason == TerminationReason.VECTORIZED],
                builder, config,
            )
            if regression:
                sources.rollback_last(source_file)
                func_result.termination_reason = TerminationReason.NO_IMPROVEMENT
                func_result.cross_function_regression = True

    # Generate cumulative patch
    patch_path = sources.generate_cumulative_patch(source_file, str(pristine_dir))
    if patch_path:
        session.final_patch_path = patch_path

    session.finished_at = time.time()
    session.total_elapsed_ms = (session.finished_at - session.started_at) * 1000
    store.save(session)

    emit_summary(results, source_file, session, store, config)

    any_vectorized = any(r.vectorized for r in results)
    return 0 if any_vectorized else 1


# ---------------------------------------------------------------------------
# Independent mode entry point
# ---------------------------------------------------------------------------

def main_independent(source_file: str, function_name: Optional[str],
                     config: DriverConfig) -> int:
    """Independent mode: compile source, get diagnostics, iterate."""
    if not Path(source_file).exists():
        print(f"[AIMV] Error: source file not found: {source_file}", file=sys.stderr)
        return 2

    builder = BuildOrchestrator(config)
    mcp = MCPClient(config.mcp_url, config.mcp_timeout, api_key=config.mcp_api_key)
    sources = SourceManager(config.output_dir)
    store = SessionStore(config.output_dir)

    # Initial compile to get diagnostics
    initial_json = str(Path(config.output_dir) / "aimv-initial.json")
    build = builder.compile_with_aimv(
        source_file=source_file,
        output_file=str(Path(config.output_dir) / "initial.o"),
        aimv_json_output=initial_json,
    )
    if build.returncode != 0:
        print(f"[AIMV] Error: initial compilation failed:\n{build.stderr}", file=sys.stderr)
        return 3

    # Parse diagnostics
    if Path(initial_json).exists():
        with open(initial_json) as f:
            aimv_data = json.load(f)
    else:
        aimv_data = opt_info_parser.parse_to_analyze_request(
            build.opt_record_path, function_name or "", source_file)

    diagnostics = aimv_data.get("diagnostics", [])

    if function_name:
        failed_functions = [function_name]
    else:
        failed_functions = list(dict.fromkeys(
            d["function_name"]
            for d in diagnostics
            if d.get("severity") == "missed"
        ))

    if not failed_functions:
        print("[AIMV] all loops already vectorized, nothing to do", file=sys.stderr)
        return 0

    # Pristine backup
    pristine_dir = Path(config.output_dir) / "backups" / "pristine"
    pristine_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_file, pristine_dir / Path(source_file).name)

    session = SessionRecord(
        source_file=source_file,
        aimv_level=config.aimv_level,
        max_rounds=config.max_rounds,
    )

    results: list[PerFunctionResult] = []

    for func_name in failed_functions:
        engine = IterationEngine(config.aimv_level, config.max_rounds)
        func_result = process_single_function(
            function_name=func_name,
            source_file=source_file,
            initial_diagnostics=[
                d for d in diagnostics
                if d["function_name"] == func_name and d.get("severity") == "missed"
            ],
            config=config,
            builder=builder,
            mcp=mcp,
            engine=engine,
            sources=sources,
            store=store,
            session=session,
        )
        results.append(func_result)
        session.functions.append(func_result)

        if func_result.termination_reason == TerminationReason.VECTORIZED:
            regression = _check_cross_function_regression(
                source_file,
                [r for r in results if r.termination_reason == TerminationReason.VECTORIZED],
                builder, config,
            )
            if regression:
                sources.rollback_last(source_file)
                func_result.termination_reason = TerminationReason.NO_IMPROVEMENT
                func_result.cross_function_regression = True

    patch_path = sources.generate_cumulative_patch(source_file, str(pristine_dir))
    if patch_path:
        session.final_patch_path = patch_path

    session.finished_at = time.time()
    session.total_elapsed_ms = (session.finished_at - session.started_at) * 1000
    store.save(session)

    emit_summary(results, source_file, session, store, config)

    any_vectorized = any(r.vectorized for r in results)
    return 0 if any_vectorized else 1


# ---------------------------------------------------------------------------
# Summary output (DRIVER_DESIGN §13)
# ---------------------------------------------------------------------------

def emit_summary(
    results: list,
    source_file: str,
    session: SessionRecord,
    store: SessionStore,
    config: DriverConfig,
):
    """Output final summary to stderr (SPEC §3.1 format)."""
    total = len(results)
    optimized = sum(1 for r in results if r.vectorized)

    if len(results) == 1:
        r = results[0]
        if r.vectorized:
            print(
                f"[AIMV] {r.function_name}: vectorized "
                f"({r.rounds_used} rounds, {session.aimv_level})",
                file=sys.stderr,
            )
            for rr in r.rounds:
                if rr.applied_diff_summary:
                    print(
                        f"[AIMV]   Round {rr.round_number}: {rr.applied_diff_summary}",
                        file=sys.stderr,
                    )
        else:
            reason_str = _format_termination(r.termination_reason, r.rounds_used)
            print(f"[AIMV] {r.function_name}: {reason_str}", file=sys.stderr)
            if r.termination_reason in (
                TerminationReason.ROUND_LIMIT,
                TerminationReason.NO_SUGGESTION,
                TerminationReason.NO_IMPROVEMENT,
            ):
                print("[AIMV]   Source rolled back to original", file=sys.stderr)
    else:
        print(
            f"[AIMV] {Path(source_file).name}: "
            f"{total} functions analyzed, "
            f"{optimized} optimized, "
            f"{total - optimized} gave up",
            file=sys.stderr,
        )
        for r in results:
            if r.vectorized:
                print(
                    f"[AIMV]   {r.function_name}: vectorized "
                    f"({r.rounds_used} rounds, {session.aimv_level})",
                    file=sys.stderr,
                )
            else:
                reason_str = _format_termination(r.termination_reason, r.rounds_used)
                print(f"[AIMV]   {r.function_name}: {reason_str}", file=sys.stderr)

    # Traceability artifacts
    patch_path = Path(source_file).resolve()
    patch_file = patch_path.parent / f"{patch_path.name}.aimv.patch"
    if patch_file.exists():
        print(f"[AIMV]   Patch: {patch_file}", file=sys.stderr)

    session_path = store.sessions_dir / f"{session.session_id}.json"
    if session_path.exists():
        print(f"[AIMV]   Report: {session_path}", file=sys.stderr)


def _format_termination(reason: TerminationReason, rounds_used: int) -> str:
    if reason == TerminationReason.ROUND_LIMIT:
        return f"unable to vectorize ({rounds_used} rounds exhausted)"
    elif reason == TerminationReason.NO_SUGGESTION:
        return "unable to vectorize (no suggestions from AI)"
    elif reason == TerminationReason.NO_IMPROVEMENT:
        return "unable to vectorize (regression detected)"
    elif reason == TerminationReason.COMPILE_ERROR:
        return "unable to vectorize (compile error)"
    elif reason == TerminationReason.TEST_FAILURE:
        return "unable to vectorize (test failure)"
    elif reason == TerminationReason.INTERRUPTED:
        return "interrupted by user"
    else:
        return f"unable to vectorize ({reason.value})"


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def build_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="aimv-driver")
    parser.add_argument("source_file", nargs="?", help="C/C++ source file")
    parser.add_argument("--from-json", dest="from_json",
                        help="Start from AIMV JSON (clang Driver fork+exec entry)")
    parser.add_argument("--source", dest="source",
                        help="Source file (used with --from-json)")
    parser.add_argument("--function", dest="function", help="Target function name")
    parser.add_argument("--aimv-level", default="conservative",
                        choices=["conservative", "moderate", "aggressive"])
    parser.add_argument("--max-rounds", type=int, default=5)
    parser.add_argument("--mcp-url", default="http://localhost:8080")
    parser.add_argument("--output-dir", default="./aimv-output")
    parser.add_argument("--test-cmd", dest="test_cmd", default="")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--resume", dest="resume")
    parser.add_argument("--list-sessions", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser


def main(argv=None):
    parser = build_argparser()
    args = parser.parse_args(argv)

    if args.verbose:
        print(f"[AIMV] args: {args}", file=sys.stderr)

    config = load_config()

    # Override config with CLI args
    if args.aimv_level:
        config.aimv_level = args.aimv_level
    if args.max_rounds:
        config.max_rounds = args.max_rounds
    if args.mcp_url:
        config.mcp_url = args.mcp_url
    if args.output_dir:
        config.output_dir = args.output_dir
    if args.test_cmd:
        config.test_cmd = args.test_cmd

    # --list-sessions
    if args.list_sessions:
        store = SessionStore(config.output_dir)
        sessions = store.list_sessions()
        if not sessions:
            print("No sessions found.")
        for s in sessions:
            print(f"  {s['session_id']}  {s.get('function_name', '')}  "
                  f"{s.get('status', 'in_progress')}  rounds={s.get('rounds', 0)}")
        return 0

    # --from-json mode
    if args.from_json:
        source_file = args.source
        if not source_file:
            print("[AIMV] Error: --source required with --from-json", file=sys.stderr)
            return 2
        if not Path(source_file).exists():
            print(f"[AIMV] Error: source file not found: {source_file}", file=sys.stderr)
            return 2
        return main_from_json(args.from_json, source_file)

    # Independent mode
    source_file = args.source_file
    if not source_file:
        print("[AIMV] Error: source_file argument required", file=sys.stderr)
        return 2

    return main_independent(source_file, args.function, config)


if __name__ == "__main__":
    sys.exit(main())
