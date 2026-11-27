#!/usr/bin/env python3
"""Focused regression tests for lipo.py gc-merge API roots."""

import contextlib
import importlib.util
import io
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace


SCRIPT = Path(__file__).with_name("lipo.py")
OPTIONAL_APIS = (
    "ejit_dump_func",
    "ejit_print_dumped",
    "ejit_print_dumped_module",
    "ejit_print_period_const_ranking",
    "ejit_print_mayconst_ranking",
    "ejit_asan_dump",
    "ejit_enable_asan_mode",
    "ejit_asan_rotation_status",
    "__ejit_asan_gate",
    "__ejit_asan_mismatch",
    "ejit_vp_get_stats",
)
RETIRED_APIS = (
    "ejit_compile_or_get",
    "ejit_dump_all",
    "ejit_taskpool_poll_one",
    "ejit_taskpool_poll_budget",
)


def find_tool(*names):
    llvm_bin = Path(os.environ.get("LLVM_BIN", r"C:\Program Files\LLVM\bin"))
    for name in names:
        tool = shutil.which(name)
        if tool:
            return Path(tool)
        candidate = llvm_bin / (name if name.endswith(".exe") else name + ".exe")
        if candidate.is_file():
            return candidate
    raise RuntimeError("missing required tool: " + " or ".join(names))


def run(command):
    return subprocess.run(
        [str(item) for item in command],
        check=True,
        capture_output=True,
        text=True,
    )


def load_lipo():
    spec = importlib.util.spec_from_file_location("ejit_lipo", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_archive(root, clang, ar, symbols, name):
    source = root / f"{name}.c"
    obj = root / f"{name}.o"
    archive = root / f"{name}.a"
    definitions = ["void ejit_init(void) {}"]
    definitions.extend(f"void {symbol}(void) {{}}" for symbol in symbols)
    definitions.append("void deliberately_unrooted(void) {}")
    source.write_text("\n".join(definitions) + "\n", encoding="ascii")
    run(
        [
            clang,
            "--target=" + os.environ.get(
                "EJIT_LIPO_TEST_TARGET", "x86_64-unknown-linux-gnu"
            ),
            "-c",
            "-ffunction-sections",
            source,
            "-o",
            obj,
        ]
    )
    run([ar, "rcs", archive, obj])
    return archive


def gc_merge(lipo, root, archive, ar, nm, ld, name):
    output = root / f"{name}_gc.a"
    build_dir = root / "empty-build"
    build_dir.mkdir(exist_ok=True)
    original_run = lipo.sp.run

    def routed_run(command, *args, **kwargs):
        command = list(command)
        if command[0] == "ar":
            command[0] = str(ar)
        elif command[0] == "nm":
            command[0] = str(nm)
        return original_run(command, *args, **kwargs)

    lipo.sp.run = routed_run
    try:
        with contextlib.redirect_stdout(io.StringIO()) as output_log:
            lipo.doit_gc_merge(
                SimpleNamespace(
                    input=str(archive),
                    output=str(output),
                    build_dir=str(build_dir),
                    ld=str(ld),
                )
            )
    finally:
        lipo.sp.run = original_run
    return output, output_log.getvalue()


def defined_symbols(nm, archive):
    output = run([nm, "-g", "--defined-only", archive]).stdout
    return {line.split()[-1] for line in output.splitlines() if line.split()}


def merge(lipo, root, archive, ld, name):
    output = root / f"{name}.o"
    with contextlib.redirect_stdout(io.StringIO()):
        lipo.doit_merge(
            SimpleNamespace(
                input=str(archive), output=str(output), build_dir=str(root), ld=str(ld)
            )
        )
    return output


def main():
    clang = find_tool("clang")
    ar = find_tool("llvm-ar", "ar")
    nm = find_tool("llvm-nm", "nm")
    ld = find_tool("ld.lld")
    lipo = load_lipo()
    with tempfile.TemporaryDirectory(prefix="ejit-lipo-roots-") as directory:
        root = Path(directory)

        test_apis = OPTIONAL_APIS + RETIRED_APIS
        complete = build_archive(root, clang, ar, test_apis, "complete")
        input_symbols = defined_symbols(nm, complete)
        if set(test_apis) - input_symbols:
            raise AssertionError(f"input archive symbols missing: {sorted(input_symbols)}")
        complete_gc, complete_log = gc_merge(
            lipo, root, complete, ar, nm, ld, "complete"
        )
        for output in (complete_gc, merge(lipo, root, complete_gc, ld, "complete")):
            complete_symbols = defined_symbols(nm, output)
            missing = set(OPTIONAL_APIS) - complete_symbols
            if missing:
                raise AssertionError(
                    f"{output.name} discarded API roots: {sorted(missing)}; "
                    f"defined={sorted(complete_symbols)}; log={complete_log!r}"
                )
            if "deliberately_unrooted" in complete_symbols:
                raise AssertionError("lipo retained an unrooted control symbol")
            if set(RETIRED_APIS) & complete_symbols:
                raise AssertionError("lipo retained a retired runtime API")

        minimal = build_archive(root, clang, ar, (), "minimal")
        minimal_gc, _ = gc_merge(lipo, root, minimal, ar, nm, ld, "minimal")
        for output in (minimal_gc, merge(lipo, root, minimal_gc, ld, "minimal")):
            minimal_symbols = defined_symbols(nm, output)
            if "ejit_init" not in minimal_symbols:
                raise AssertionError("mandatory ejit_init root was discarded")
            if set(OPTIONAL_APIS) & minimal_symbols:
                raise AssertionError("lipo fabricated missing optional symbols")

    print("lipo GC-root regression: PASS")


if __name__ == "__main__":
    main()
