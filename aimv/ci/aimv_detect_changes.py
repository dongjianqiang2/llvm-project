#!/usr/bin/env python3
# [AIMV] AIMV CI — aimv-detect-changes CLI (T6.1)
"""Detect changed functions via git diff + AST analysis."""
import argparse
import json
import sys
from .change_detector import get_changed_functions, filter_loopy_functions


def main():
    parser = argparse.ArgumentParser(prog="aimv-detect-changes")
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--target", default="HEAD")
    parser.add_argument("--output", help="Output JSON file")
    parser.add_argument("--filter-loops", action="store_true",
                        help="Only include functions containing loops")
    args = parser.parse_args()

    funcs = get_changed_functions(args.base, args.target)
    if args.filter_loops:
        funcs = filter_loopy_functions(funcs)

    result = [
        {"file": f, "function": n, "start_line": s, "end_line": e}
        for f, n, s, e in funcs
    ]

    if args.output:
        with open(args.output, "w") as fp:
            json.dump(result, fp, indent=2)
    else:
        json.dump(result, sys.stdout, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
