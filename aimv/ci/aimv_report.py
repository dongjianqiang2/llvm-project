#!/usr/bin/env python3
# [BiSheng] AIMV CI — aimv-report (T6.3)
"""Generate AIMV analysis report from session JSON files."""
import argparse
import json
import sys
from pathlib import Path
from datetime import datetime


def generate_markdown(results_dir: str) -> str:
    """Generate Markdown report from session JSON files."""
    sessions_dir = Path(results_dir) / "sessions"
    sessions = []
    if sessions_dir.exists():
        for path in sorted(sessions_dir.glob("*.json")):
            with open(path) as f:
                sessions.append(json.load(f))

    lines = []
    lines.append("<!-- AIMV Vectorization Analysis -->\n")
    lines.append("## AIMV Vectorization Analysis Report\n")

    total = len(sessions)
    vectorized = sum(1 for s in sessions
                     if s.get("termination_reason") == "vectorized")
    gave_up = sum(1 for s in sessions
                  if s.get("termination_reason") in ("round_limit", "gave_up"))
    no_suggestion = sum(1 for s in sessions
                        if s.get("termination_reason") == "no_suggestion")

    lines.append("| Metric | Value |")
    lines.append("|--------|-------|")
    lines.append(f"| Functions analyzed | {total} |")
    lines.append(f"| Successfully vectorized | {vectorized} |")
    lines.append(f"| No suggestion available | {no_suggestion} |")
    lines.append(f"| Gave up | {gave_up} |")
    success_rate = (vectorized / max(total, 1)) * 100
    lines.append(f"| Success rate | {success_rate:.0f}% |\n")

    lines.append("### Details\n")
    for s in sessions:
        func = s.get("function_name", "unknown")
        reason = s.get("termination_reason", "in_progress")
        rounds = len(s.get("rounds", []))
        perf = s.get("overall_perf_improvement_pct")

        status_icon = {"vectorized": "+", "gave_up": "-!",
                       "no_suggestion": "(i)", "round_limit": "-!"}.get(reason, "?")

        lines.append(f"<details>")
        lines.append(f"<summary><b>{func}</b> — {reason} {status_icon}</summary>\n")
        lines.append(f"- Status: {reason}")
        lines.append(f"- Rounds: {rounds}")
        if perf:
            lines.append(f"- Perf improvement: {perf:.1f}%")
        lines.append(f"</details>\n")

    lines.append(f"---\n")
    lines.append(f"*Report generated: {datetime.now().isoformat()}*")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(prog="aimv-report")
    parser.add_argument("--input", required=True, help="Results directory")
    parser.add_argument("--format", choices=["markdown", "json"], default="markdown")
    parser.add_argument("--output", help="Output file (default: stdout)")
    args = parser.parse_args()

    if args.format == "markdown":
        output = generate_markdown(args.input)
    else:
        # JSON: collect all session summaries
        sessions_dir = Path(args.input) / "sessions"
        data = []
        if sessions_dir.exists():
            for path in sessions_dir.glob("*.json"):
                with open(path) as f:
                    data.append(json.load(f))
        output = json.dumps(data, indent=2)

    if args.output:
        Path(args.output).write_text(output)
    else:
        print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
