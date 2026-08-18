#!/usr/bin/env python3
"""
Parse EJIT dump logs: extract post-link hex + pre-link ASM, disassemble, compare.

Usage:
    ./parse_dump.py <log_file> [--objdump <path>] [--outdir <dir>] [--name <func>]

Parses a board log containing:
  - ejit_print_dumped("name")       -> pre-link .s ASM (dump ASM:N: <text>)
  - ejit_print_dumped_code("name")  -> post-link hex (  00000000: 1f 00 00 91)

For each function found:
  1. Extracts the post-link hex, writes a .bin file
  2. Disassembles with objdump (auto-detected or --objdump)
  3. Extracts the pre-link ASM, writes a .s file
  4. Prints a summary (instruction counts, stub count, addressing mode)
  5. Optionally prints side-by-side comparison

The script requires a GNU AArch64 objdump because llvm-objdump does not accept
raw binary input. It tries --objdump, then $EJIT_OBJDUMP, then the common GNU
cross-objdump names.
"""

import argparse
import os
import re
import subprocess
import sys
from collections import namedtuple
from pathlib import Path

# ── Data structures ──────────────────────────────────────────────────────────

DumpData = namedtuple("DumpData", ["name", "hex_bytes", "pre_asm", "code_size_log"])

# ── Log parsing ──────────────────────────────────────────────────────────────

# Board-log exports may collapse all records onto one physical line. Split on
# timestamps first so each prefixed EJIT record can be parsed independently.
LOG_TIMESTAMP_RE = re.compile(r"(?=\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\])")

# Post-link hex line: "  00000000: 1f 00 00 91" (with optional prefix)
HEX_LINE_RE = re.compile(
    r"\b[0-9a-fA-F]{8,16}:\s+([0-9a-fA-F]{2})\s+([0-9a-fA-F]{2})\s+([0-9a-fA-F]{2})\s+([0-9a-fA-F]{2})(?![0-9a-fA-F])"
)
HEX_HEADER_RE = re.compile(
    r"post-link code hex dump\s+name=(\S+)\s+start=0x([0-9a-fA-F]+)\s+"
    r"fn=0x([0-9a-fA-F]+)\s+fn_offset=0x([0-9a-fA-F]+)\s+size=(\d+)"
)
HEX_END_RE = re.compile(r"post-link code hex dump end")

# Pre-link ASM line: "dump ASM:123: <text>" (with optional prefix)
ASM_LINE_RE = re.compile(r"dump\s+ASM:\d+:\s?(.*)$")
ASM_HEADER_RE = re.compile(r"dump ASM begin\s+size=\d+")
ASM_END_RE = re.compile(r"dump ASM end")

# Code size log from compileCold
CODE_SIZE_RE = re.compile(
    r"dump:\s+codeStart=0x([0-9a-fA-F]+)\s+codeSize=(\d+)\s+poolBase=0x([0-9a-fA-F]+)\s+poolSize=(\d+)"
)

# Compile OK line (has func name + pfn)
COMPILE_OK_RE = re.compile(r"compile OK.*func=(\S+).*pfn=(0x[0-9a-fA-F]+)")


def parse_log(log_path):
    """Parse the log file, return dict[name] -> DumpData."""
    funcs = {}  # name -> {hex_bytes, pre_asm, code_size_log, pfn}

    with open(log_path, "r", errors="replace") as f:
        text = f.read()

    # Preserve ordinary line-oriented logs while also handling copied logs in
    # which the collector removed all newlines between timestamped records.
    lines = []
    for physical_line in text.splitlines():
        lines.extend(part for part in LOG_TIMESTAMP_RE.split(physical_line) if part)

    state = None  # None | "hex" | "asm"
    cur_name = None

    for line in lines:
        # ── Detect code size log ──
        m = CODE_SIZE_RE.search(line)
        if m:
            cs = int(m.group(2))
            # Store against whatever function was last compiled
            # (will be associated by name below)
            for name in funcs:
                if "code_size_log" not in funcs[name] or not funcs[name]["code_size_log"]:
                    funcs[name]["code_size_log"] = {
                        "codeStart": int(m.group(1), 16),
                        "codeSize": cs,
                        "poolBase": int(m.group(3), 16),
                        "poolSize": int(m.group(4), 16),
                    }
            continue

        # ── Detect compile OK (capture pfn) ──
        m = COMPILE_OK_RE.search(line)
        if m:
            name = m.group(1)
            pfn = m.group(2)
            if name not in funcs:
                funcs[name] = {
                    "hex_bytes": b"",
                    "pre_asm": [],
                    "code_size_log": None,
                    "pfn": pfn,
                }
            else:
                funcs[name]["pfn"] = pfn
            continue

        # ── Post-link hex dump ──
        m = HEX_HEADER_RE.search(line)
        if m:
            state = "hex"
            cur_name = m.group(1)
            if cur_name not in funcs:
                funcs[cur_name] = {
                    "hex_bytes": b"",
                    "hex_size_log": None,
                    "pre_asm": [],
                    "code_size_log": None,
                    "pfn": None,
                }
            funcs[cur_name]["hex_bytes"] = b""
            funcs[cur_name]["code_start"] = int(m.group(2), 16)
            funcs[cur_name]["pfn"] = f"0x{int(m.group(3), 16):x}"
            funcs[cur_name]["fn_offset"] = int(m.group(4), 16)
            funcs[cur_name]["hex_size_log"] = int(m.group(5))
            continue

        if state == "hex":
            if HEX_END_RE.search(line):
                state = None
                cur_name = None
                continue
            m = HEX_LINE_RE.search(line)
            if m and cur_name:
                byte_vals = bytes(int(g, 16) for g in m.groups())
                funcs[cur_name]["hex_bytes"] += byte_vals
            continue

        # ── Pre-link ASM dump ──
        if ASM_HEADER_RE.search(line):
            state = "asm"
            continue

        if state == "asm":
            if ASM_END_RE.search(line):
                state = None
                continue
            m = ASM_LINE_RE.search(line)
            if m:
                # Associate with the most recently compiled function
                # (or the one set by ejit_dump_func)
                target = cur_name or (list(funcs.keys())[-1] if funcs else None)
                if target:
                    if target not in funcs:
                        funcs[target] = {
                            "hex_bytes": b"",
                            "hex_size_log": None,
                            "pre_asm": [],
                            "code_size_log": None,
                            "pfn": None,
                        }
                    funcs[target]["pre_asm"].append(m.group(1).rstrip())
            continue

    return funcs


# ── Objdump detection ────────────────────────────────────────────────────────

def find_objdump(explicit=None):
    """Find an objdump that can disassemble AArch64 (LE instructions)."""
    candidates = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("EJIT_OBJDUMP")
    if env:
        candidates.append(env)
    # System cross objdumps
    for name in [
        "aarch64-linux-gnu-objdump",
        "aarch64_be-linux-gnu-objdump",
    ]:
        import shutil

        path = shutil.which(name)
        if path:
            candidates.append(path)

    for c in candidates:
        if "llvm-objdump" in Path(c).name:
            continue
        try:
            r = subprocess.run(
                [c, "--version"], capture_output=True, text=True, timeout=5
            )
            if r.returncode == 0:
                return c
        except Exception:
            pass
    return None


# ── Disassembly ──────────────────────────────────────────────────────────────

def disassemble(hex_bytes, objdump, base_addr=0):
    """Disassemble raw bytes, return list of (addr_hex, bytes_hex, mnemonic)."""
    import tempfile

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(hex_bytes)
        bin_path = f.name

    try:
        cmd = [
            objdump,
            "-D",
            "-b",
            "binary",
            "-m",
            "aarch64",
            f"--adjust-vma=0x{base_addr:x}",
            bin_path,
        ]

        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            raise RuntimeError(
                f"objdump failed ({r.returncode}): {r.stderr.strip()}"
            )
        return r.stdout
    finally:
        os.unlink(bin_path)


# ── Stub detection ───────────────────────────────────────────────────────────

# AArch64 instruction encodings (LE in memory):
#   ADRP x16, ...: 0x90xxxxx0 (bit pattern: 1 immlo[2] 10000 immhi[19] Rd[5]=10000)
#   LDR  x16, [x16, #...]: 0xF940xxxx (1111100101 imm12 Rn=10000 Rt=10000)
#   BR   x16: 0xD61F0200 (1101011 0000 1 1111100000 0 Rn=10000 00000)
ADRP_X16 = re.compile(r"adrp\s+x16", re.IGNORECASE)
LDR_X16_FROM_X16 = re.compile(r"ldr\s+x16,\s*\[x16", re.IGNORECASE)
BR_X16 = re.compile(r"^\s*[0-9a-f]+:\s+.*br\s+x16", re.IGNORECASE)


def count_stubs(disasm_text):
    """Count PointerJumpStub patterns (ADRP x16 + LDR x16 + BR x16) in disasm."""
    lines = disasm_text.split("\n")
    stub_count = 0
    stub_addrs = []
    i = 0
    while i < len(lines) - 2:
        l1, l2, l3 = lines[i], lines[i + 1], lines[i + 2]
        if (
            ADRP_X16.search(l1)
            and LDR_X16_FROM_X16.search(l2)
            and BR_X16.search(l3)
        ):
            stub_count += 1
            # Extract address from first line
            m = re.match(r"\s*([0-9a-f]+):", l1)
            if m:
                stub_addrs.append(int(m.group(1), 16))
            i += 3
        else:
            i += 1
    return stub_count, stub_addrs


# ── Addressing mode detection ────────────────────────────────────────────────

MOVZ_ABS_RE = re.compile(r"movz\s+\w+,\s+#:abs_g", re.IGNORECASE)
ADRP_RE = re.compile(r"adrp\s+", re.IGNORECASE)
GOT_RE = re.compile(r":got:", re.IGNORECASE)


def analyze_addressing(disasm_text):
    """Detect addressing modes used."""
    movz_count = len(MOVZ_ABS_RE.findall(disasm_text))
    adrp_count = len(ADRP_RE.findall(disasm_text))
    got_count = len(GOT_RE.findall(disasm_text))
    return {
        "movz_abs": movz_count,
        "adrp": adrp_count,
        "got": got_count,
    }


# ── BL call detection ────────────────────────────────────────────────────────

BL_RE = re.compile(r"^\s*[0-9a-f]+:\s+.*\bbl\s+", re.IGNORECASE)
TRANSFER_RE = re.compile(
    r"^\s*[0-9a-f]+:\s+.*\b(?:b|bl|br|blr)\b.*$", re.IGNORECASE
)


def count_bl(disasm_text):
    """Count BL instructions (function calls)."""
    return len(BL_RE.findall(disasm_text))


def control_transfers(disasm_text):
    """Return direct and indirect branch/call instructions for inspection."""
    return [line.strip() for line in disasm_text.splitlines() if TRANSFER_RE.match(line)]


# ── Output ───────────────────────────────────────────────────────────────────

def process_function(name, data, objdump, outdir):
    """Process one function: disassemble, analyze, print summary."""
    print(f"\n{'='*72}")
    print(f"Function: {name}")
    print(f"{'='*72}")

    pfn = data.get("pfn")
    code_start = data.get("code_start")
    fn_offset = data.get("fn_offset")
    csl = data.get("code_size_log")
    hex_bytes = data.get("hex_bytes", b"")
    hex_size_log = data.get("hex_size_log")
    pre_asm = data.get("pre_asm", [])

    if csl:
        print(f"  codeStart=0x{csl['codeStart']:x}  codeSize={csl['codeSize']}")
        print(f"  poolBase=0x{csl['poolBase']:x}  poolSize={csl['poolSize']}")
    if pfn:
        print(f"  pfn={pfn}")
    if code_start is not None:
        print(f"  dumpStart=0x{code_start:x}  fnOffset=0x{fn_offset:x}")

    # Pre-link ASM
    if pre_asm:
        print(f"\n  Pre-link ASM: {len(pre_asm)} lines")
        asm_path = outdir / f"{name}_prelink.s"
        with open(asm_path, "w") as f:
            f.write("\n".join(pre_asm) + "\n")
        print(f"    -> {asm_path}")
    else:
        print("\n  Pre-link ASM: (none)")

    # Post-link hex
    if not hex_bytes:
        print("  Post-link hex: (none)")
        return

    print(f"  Post-link hex: {len(hex_bytes)} bytes")
    if hex_size_log is not None and len(hex_bytes) != hex_size_log:
        print(
            f"  WARNING: truncated/incomplete dump: expected {hex_size_log} "
            f"bytes, parsed {len(hex_bytes)}"
        )

    # Write binary
    bin_path = outdir / f"{name}_postlink.bin"
    with open(bin_path, "wb") as f:
        f.write(hex_bytes)
    print(f"    -> {bin_path}")

    # Disassemble
    base = code_start or 0
    if not base and csl:
        base = csl["codeStart"]

    disasm = disassemble(hex_bytes, objdump, base)
    disasm_path = outdir / f"{name}_postlink.disasm"
    with open(disasm_path, "w") as f:
        f.write(disasm)
    print(f"  Disasm: -> {disasm_path}")

    # Analysis
    stubs, stub_addrs = count_stubs(disasm)
    addr_modes = analyze_addressing(disasm)
    bl_count = count_bl(disasm)
    transfers = control_transfers(disasm)

    # Count total instructions in disasm
    insn_lines = [
        l for l in disasm.split("\n") if re.match(r"^\s*[0-9a-f]+:\s+[0-9a-f]{8}", l)
    ]

    print(f"\n  ── Analysis ──")
    print(f"  Total instructions (post-link): {len(insn_lines)}")
    print(f"  Pre-link ASM lines:             {len(pre_asm)}")
    print(f"  BL calls:                       {bl_count}")
    print(f"  Stubs (ADRP+LDR+BR x16):        {stubs}")
    if stub_addrs:
        for a in stub_addrs:
            print(f"    @ 0x{a:x}")
    print(f"  Addressing: ADRP={addr_modes['adrp']}  movz/abs={addr_modes['movz_abs']}  GOT={addr_modes['got']}")
    if transfers:
        print("\n  -- Branch/call instructions --")
        for line in transfers:
            print(f"  {line}")

    if stubs > 0:
        stub_cycles = stubs * 5  # rough: ~5c per stub (2 insns + 1 mem + BR)
        print(f"\n  ⚠ {stubs} stubs ≈ ~{stub_cycles}c overhead (each: ADRP+LDR+BR via GOT)")
        print(f"    (AOT has 0 stubs - direct BL to .text)")

    # Print first 30 lines of disasm for quick view
    print(f"\n  ── First 30 lines of post-link disasm ──")
    for line in disasm.split("\n")[:30]:
        print(f"  {line}")
    if len(insn_lines) > 30:
        print(f"  ... ({len(insn_lines) - 30} more)")


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Parse EJIT dump logs, disassemble post-link code, compare."
    )
    parser.add_argument("log_file", help="Board log file containing dump output")
    parser.add_argument(
        "--objdump", default=None, help="Path to objdump (auto-detected if omitted)"
    )
    parser.add_argument(
        "--outdir", default=None, help="Output directory (default: <log>_out/)"
    )
    parser.add_argument(
        "--name", default=None, help="Process only this function (default: all)"
    )
    args = parser.parse_args()

    log_path = Path(args.log_file)
    if not log_path.exists():
        print(f"Error: {log_path} not found", file=sys.stderr)
        sys.exit(1)

    outdir = Path(args.outdir) if args.outdir else log_path.parent / f"{log_path.stem}_out"
    outdir.mkdir(parents=True, exist_ok=True)

    objdump = find_objdump(args.objdump)
    if not objdump:
        print(
            "Error: no GNU AArch64 objdump found. Use --objdump or set "
            "$EJIT_OBJDUMP. llvm-objdump cannot disassemble raw binary input.",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f"Using objdump: {objdump}")

    funcs = parse_log(log_path)
    if not funcs:
        print("No dump data found in log.", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(funcs)} function(s): {', '.join(funcs.keys())}")

    for name, data in funcs.items():
        if args.name and args.name != name:
            continue
        process_function(name, data, objdump, outdir)

    print(f"\nOutput files in: {outdir}/")


if __name__ == "__main__":
    main()
