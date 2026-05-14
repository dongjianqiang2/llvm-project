# [BiSheng] AIMV CI — Incremental change detection (T6.1)
"""Detect changed functions in a git diff for incremental AIMV analysis."""
import subprocess
import re
from pathlib import Path
from typing import List, Tuple


def get_changed_functions(
    base_branch: str = "origin/main",
    target_branch: str = "HEAD",
) -> List[Tuple[str, str, int, int]]:
    """Return list of (file, function_name, start_line, end_line) tuples."""
    # 1. Get changed C files
    proc = subprocess.run(
        ["git", "diff", "--name-only", f"{base_branch}...{target_branch}"],
        capture_output=True, text=True,
    )
    changed_files = [
        f for f in proc.stdout.strip().split("\n")
        if f.endswith((".c", ".cpp", ".cxx", ".cc"))
    ]

    if not changed_files:
        return []

    # 2. For each file, find affected functions
    functions = []
    for file in changed_files:
        if not Path(file).exists():
            continue
        hunk_ranges = _get_changed_line_ranges(file, base_branch, target_branch)
        funcs_in_file = _get_function_definitions(file)
        for func_name, func_start, func_end in funcs_in_file:
            for hunk_start, hunk_end in hunk_ranges:
                if _ranges_overlap(func_start, func_end, hunk_start, hunk_end):
                    functions.append((file, func_name, func_start, func_end))
                    break

    return functions


def _get_function_definitions(file: str) -> List[Tuple[str, int, int]]:
    """Use clang AST dump to find function definitions."""
    cmd = ["clang-check", "-ast-dump", file, "--", "-fsyntax-only"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return _get_function_definitions_regex(file)

    functions = []
    for line in proc.stderr.split("\n"):
        if "FunctionDecl" in line and " definition " in line:
            name_match = re.search(r"FunctionDecl.*?\b(\w+)\b.*?'", line)
            loc_match = re.search(r"<line:(\d+):\d+, line:(\d+):\d+>", line)
            if name_match and loc_match:
                functions.append(
                    (name_match.group(1), int(loc_match.group(1)),
                     int(loc_match.group(2))))
    return functions


def _get_function_definitions_regex(file: str) -> List[Tuple[str, int, int]]:
    """Regex-based fallback for function detection."""
    text = Path(file).read_text()
    functions = []
    pattern = re.compile(
        r'^(?:static\s+|inline\s+|extern\s+)*[\w\s*]+'
        r'(\w+)\s*\([^)]*\)\s*\{', re.MULTILINE)
    for m in pattern.finditer(text):
        name = m.group(1)
        line_start = text[:m.start()].count('\n') + 1
        functions.append((name, line_start, line_start + 50))
    return functions


def _get_changed_line_ranges(
    file: str, base: str, target: str
) -> List[Tuple[int, int]]:
    """Parse git diff hunk headers to get changed line ranges."""
    proc = subprocess.run(
        ["git", "diff", f"{base}...{target}", "--", file],
        capture_output=True, text=True,
    )
    ranges = []
    for line in proc.stdout.split("\n"):
        if line.startswith("@@"):
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            if m:
                start = int(m.group(1))
                count = int(m.group(2)) if m.group(2) else 1
                ranges.append((start, start + count - 1))
    return ranges


def _ranges_overlap(a1: int, a2: int, b1: int, b2: int) -> bool:
    return max(a1, b1) <= min(a2, b2)


def filter_loopy_functions(
    functions: List[Tuple[str, str, int, int]], cc: str = "clang"
) -> List[Tuple[str, str, int, int]]:
    """Pre-filter: only keep functions that contain loops."""
    import tempfile, os
    loopy = []
    files_processed = set()
    for file, func_name, start, end in functions:
        if file in files_processed:
            continue
        files_processed.add(file)
        with tempfile.NamedTemporaryFile(suffix=".ll", delete=False) as tmp:
            ir_path = tmp.name
        try:
            subprocess.run(
                [cc, "-S", "-emit-llvm", "-O0", file, "-o", ir_path],
                capture_output=True, text=True, timeout=30,
            )
            proc = subprocess.run(
                ["opt", "-passes=print<loops>", "-disable-output", ir_path],
                capture_output=True, text=True, timeout=30,
            )
            loopy_funcs = {f[0] for f in loopy}
            for fn, fname, fs, fe in functions:
                if fn not in loopy_funcs and fname in proc.stderr:
                    loopy.append((fn, fname, fs, fe))
        finally:
            os.unlink(ir_path)
    return loopy
