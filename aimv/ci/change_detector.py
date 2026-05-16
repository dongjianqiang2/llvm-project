# [AIMV] AIMV CI — Incremental change detection (T6.1)
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

    # [AIMV] AST dump format:
    #   |-FunctionDecl 0x7f8b4c401600 <line:10:1, line:20:1> line:10:5 foo 'int (int)'
    #   `-FunctionDecl 0x... prev 0x... <col:...> used bar 'void ()'
    # The function name is the last bare identifier before the quoted type string.
    # Pattern: capture the identifier immediately before 'type' (single-quoted).
    # Fallback: capture identifier after source location "line:N:C ".
    functions = []
    for line in proc.stderr.split("\n"):
        if "FunctionDecl" not in line or " definition " not in line:
            continue
        loc_match = re.search(r"<line:(\d+):\d+, line:(\d+):\d+>", line)
        if not loc_match:
            continue
        # Prefer: word right before 'type' quote, e.g. "... foo 'int ()'"
        name_match = re.search(r"\b(\w+)\s+'[^']*'", line)
        if not name_match:
            # Fallback: word after "line:N:C" source location
            name_match = re.search(r"line:\d+:\d+\s+(\w+)", line)
        if name_match:
            functions.append(
                (name_match.group(1), int(loc_match.group(1)),
                 int(loc_match.group(2))))
    return functions


def _get_function_definitions_regex(file: str) -> List[Tuple[str, int, int]]:
    """Regex-based fallback for function detection.

    Matches C function definitions by looking for the pattern:
      [qualifiers] return_type func_name(params) {
    Key: capture the identifier immediately before '(' that is NOT
    a C keyword. The '(' is the reliable anchor — everything after
    the last '(' on a definition line is the parameter list.
    """
    text = Path(file).read_text()
    functions = []
    # [AIMV] Strategy: find lines ending with '{' where an identifier
    # precedes a parenthesized parameter list. Exclude C keywords.
    keywords = frozenset({
        "if", "else", "for", "while", "do", "switch", "return",
        "struct", "union", "enum", "typedef", "sizeof", "case",
    })
    pattern = re.compile(
        r'^[ \t]*(?:(?:static|inline|extern|const|unsigned|signed|void'
        r'|int|long|short|char|float|double|struct\s+\w+'
        r'|enum\s+\w+|union\s+\w+)\s+)*'
        r'(\*?\s*\w+)\s*\(([^)]*)\)\s*\{',
        re.MULTILINE,
    )
    for m in pattern.finditer(text):
        name = m.group(1).strip().lstrip("*").strip()
        if not name or name in keywords:
            continue
        line_start = text[:m.start()].count('\n') + 1
        # [AIMV] Find matching closing brace for end_line
        depth = 1
        pos = m.end()
        end_line = line_start
        while pos < len(text) and depth > 0:
            if text[pos] == '{':
                depth += 1
            elif text[pos] == '}':
                depth -= 1
            elif text[pos] == '\n':
                end_line += 1
            pos += 1
        functions.append((name, line_start, end_line))
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
    files_processed = {}  # file -> set of function names found in opt output
    for file, func_name, start, end in functions:
        if file not in files_processed:
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
                # [AIMV] Extract function names from loop printer output.
                # Format: "Loop at depth N containing: ...\n  in function: func_name"
                found = set()
                for fm in re.finditer(r"in function:\s*(\w+)", proc.stderr):
                    found.add(fm.group(1))
                files_processed[file] = found
            finally:
                os.unlink(ir_path)
        # [AIMV] Check only functions from the SAME file
        if func_name in files_processed.get(file, set()):
            loopy.append((file, func_name, start, end))
    return loopy
