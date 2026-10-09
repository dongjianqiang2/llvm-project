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
DUMP_APIS = (
    "ejit_dump_func",
    "ejit_print_dumped",
    "ejit_print_dumped_module",
)
SMALL_TABLE_HOOKS = (
    "ejit_stab_enter",
    "ejit_stab_wrapper_enter",
    "ejit_stab_wrapper_no_policy_current",
    "ejit_stab_leave",
    "ejit_stab_dispatch",
    "ejit_small_table_host_installed",
    "ejit_small_table_published_slots",
)
SMALL_TABLE_SRE_HOOKS = (
    "ejit_small_table_sre_prepare",
    "ejit_small_table_sre_prepare_data",
    "ejit_small_table_sre_request",
    "ejit_small_table_sre_get_snapshot",
    "ejit_small_table_sre_finish",
    "ejit_small_table_sre_cancel",
    "ejit_small_table_sre_print",
)
SMALL_TABLE_DEMO = (
    "test_ejit_period",
    "test_ejit_smalltable_print",
    "g_pr231_probe_dispatch",
    "pr231_probe_inflight",
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


def build_archive(root, clang, ar, symbols, name, registration_fixture=False):
    source = root / f"{name}.c"
    obj = root / f"{name}.o"
    archive = root / f"{name}.a"
    definitions = ["void ejit_init(void) {}"]
    if "g_pr231_probe_dispatch" in symbols:
        # A real object-typed mutable shared callback slot, not a function
        # placeholder. The target checker separately validates its placement.
        definitions.append("void pr231_probe_inflight(void);")
    for symbol in symbols:
        if symbol == "g_pr231_probe_dispatch":
            definitions.append(
                '__attribute__((used,section(".mc_shared"))) '
                'void (* volatile g_pr231_probe_dispatch)(void) = '
                'pr231_probe_inflight;'
            )
        else:
            definitions.append(f"void {symbol}(void) {{}}")
    definitions.append("void deliberately_unrooted(void) {}")
    if registration_fixture:
        definitions.extend(
            [
                '__attribute__((used,section(".ejit_bitcode"))) '
                'const unsigned char bitcode_registration[] = {0x45, 0x4a};',
                '__attribute__((used,section(".ejit_period"))) '
                'const unsigned char period_registration[] = {0x50, 0x52};',
                '__attribute__((used,section(".mc_shared"))) '
                'unsigned int shared_core_state = 7;',
                'static void registered_constructor(void) { shared_core_state++; }',
                '__attribute__((used,section(".init_array.00101"))) '
                'void (*const constructor_registration)(void) = registered_constructor;',
            ]
        )
    source.write_text("\n".join(definitions) + "\n", encoding="ascii")
    run(
        [
            clang,
            "-c",
            "-ffreestanding",
            "-ffunction-sections",
            source,
            "-o",
            obj,
        ]
    )
    run([ar, "rcs", archive, obj])
    return archive


def gc_merge(lipo, root, archive, ar, nm, ld, name, *,
             require_small_table=False, require_small_table_sre=False,
             require_demo=False):
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
            lipo.doit_gc_merge(SimpleNamespace(
                input=str(archive), output=str(output),
                build_dir=str(build_dir), ld=str(ld),
                require_small_table=require_small_table,
                require_small_table_sre=require_small_table_sre,
                require_demo=require_demo,
            ))
    except BaseException as error:
        error.captured_stdout = output_log.getvalue()
        raise
    finally:
        lipo.sp.run = original_run
    return output, output_log.getvalue()


def merge(lipo, root, archive, nm, ld, name, *,
          require_small_table_sre=False, require_demo=False):
    output = root / f"{name}.o"
    build_dir = root / "empty-build"
    original_run = lipo.sp.run

    def routed_run(command, *args, **kwargs):
        command = list(command)
        if command[0] == "nm":
            command[0] = str(nm)
        return original_run(command, *args, **kwargs)

    lipo.sp.run = routed_run
    try:
        with contextlib.redirect_stdout(io.StringIO()) as output_log:
            lipo.doit_merge(SimpleNamespace(
                input=str(archive), output=str(output),
                build_dir=str(build_dir), ld=str(ld),
                require_small_table=False,
                require_small_table_sre=require_small_table_sre,
                require_demo=require_demo,
            ))
    finally:
        lipo.sp.run = original_run
    return output, output_log.getvalue()


def build_duplicate_root_archive(root, clang, ar, name):
    objects = []
    for index in range(2):
        source = root / f"{name}_{index}.c"
        obj = root / f"{name}_{index}.o"
        source.write_text(
            "void ejit_init(void) {}\n"
            "void ejit_stab_enter(void) {}\n",
            encoding="ascii",
        )
        run([clang, "-c", "-ffreestanding", "-ffunction-sections",
             source, "-o", obj])
        objects.append(obj)
    archive = root / f"{name}.a"
    run([ar, "rcs", archive, *objects])
    return archive


def defined_symbols(nm, archive):
    output = run([nm, "-g", "--defined-only", archive]).stdout
    return {line.split()[-1] for line in output.splitlines() if line.split()}


def main():
    clang = find_tool("clang")
    ar = find_tool("llvm-ar", "ar")
    nm = find_tool("llvm-nm", "nm")
    ld = find_tool("ld.lld")
    lipo = load_lipo()
    with tempfile.TemporaryDirectory(prefix="ejit-lipo-roots-") as directory:
        root = Path(directory)

        complete = build_archive(root, clang, ar, DUMP_APIS, "complete")
        input_symbols = defined_symbols(nm, complete)
        if set(DUMP_APIS) - input_symbols:
            raise AssertionError(f"input archive symbols missing: {sorted(input_symbols)}")
        complete_gc, complete_log = gc_merge(
            lipo, root, complete, ar, nm, ld, "complete"
        )
        complete_symbols = defined_symbols(nm, complete_gc)
        missing = set(DUMP_APIS) - complete_symbols
        if missing:
            raise AssertionError(
                f"dump API roots were discarded: {sorted(missing)}; "
                f"defined={sorted(complete_symbols)}; log={complete_log!r}"
            )
        if "deliberately_unrooted" in complete_symbols:
            raise AssertionError("gc-merge retained an unrooted control symbol")

        runtime_only = build_archive(
            root, clang, ar, (*SMALL_TABLE_HOOKS, *SMALL_TABLE_SRE_HOOKS),
            "runtime-only", registration_fixture=True,
        )
        runtime_only_gc, runtime_only_log = gc_merge(
            lipo, root, runtime_only, ar, nm, ld, "runtime-only",
            require_small_table_sre=True,
        )
        runtime_only_symbols = defined_symbols(nm, runtime_only_gc)
        runtime_roots = set(SMALL_TABLE_HOOKS + SMALL_TABLE_SRE_HOOKS)
        if runtime_roots - runtime_only_symbols:
            raise AssertionError(
                "runtime-only --require-small-table-sre discarded a required "
                f"hook: {sorted(runtime_roots - runtime_only_symbols)}; "
                f"log={runtime_only_log!r}"
            )
        if set(SMALL_TABLE_DEMO) & runtime_only_symbols:
            raise AssertionError("runtime-only gc-merge fabricated demo roots")
        runtime_only_merged, _ = merge(
            lipo, root, runtime_only_gc, nm, ld, "runtime-only-merged",
            require_small_table_sre=True,
        )
        runtime_merged_symbols = defined_symbols(nm, runtime_only_merged)
        if runtime_roots - runtime_merged_symbols:
            raise AssertionError(
                "runtime-only --require-small-table-sre merge discarded a "
                f"required hook: {sorted(runtime_roots - runtime_merged_symbols)}"
            )
        if set(SMALL_TABLE_DEMO) & runtime_merged_symbols:
            raise AssertionError("runtime-only merge fabricated demo roots")

        smalltable = build_archive(
            root, clang, ar,
            (*SMALL_TABLE_HOOKS, *SMALL_TABLE_SRE_HOOKS, *SMALL_TABLE_DEMO),
            "smalltable", registration_fixture=True,
        )
        smalltable_gc, smalltable_log = gc_merge(
            lipo, root, smalltable, ar, nm, ld, "smalltable",
            require_small_table_sre=True, require_demo=True,
        )
        smalltable_symbols = defined_symbols(nm, smalltable_gc)
        expected_roots = set(SMALL_TABLE_HOOKS + SMALL_TABLE_SRE_HOOKS + SMALL_TABLE_DEMO)
        if expected_roots - smalltable_symbols:
            raise AssertionError(
                "small-table runtime/demo roots were discarded: "
                f"{sorted(expected_roots - smalltable_symbols)}; "
                f"log={smalltable_log!r}"
            )

        smalltable_sections = lipo._readelf_sections(smalltable_gc, str(root / "empty-build"))
        for section in (".ejit_bitcode", ".ejit_period", ".mc_shared", ".init_array"):
            if section not in smalltable_sections and not any(
                    name.startswith(section + ".") for name in smalltable_sections):
                raise AssertionError(f"gc-merge discarded retained section {section}")

        smalltable_merged, merge_log = merge(
            lipo, root, smalltable_gc, nm, ld, "smalltable-merged",
            require_small_table_sre=True, require_demo=True,
        )
        merged_symbols = defined_symbols(nm, smalltable_merged)
        if expected_roots - merged_symbols:
            raise AssertionError(
                "final merge discarded small-table roots: "
                f"{sorted(expected_roots - merged_symbols)}; log={merge_log!r}"
            )
        merged_sections = lipo._readelf_sections(
            smalltable_merged, str(root / "empty-build"))
        if not {".symtab", ".strtab", ".rodata", ".mc_shared", ".init_array"} <= merged_sections:
            raise AssertionError(
                f"final merge lost symbol/registration/shared sections: "
                f"{sorted(merged_sections)}"
            )
        for prefix in ("ejit_bitcode", "ejit_period"):
            if f"__start_{prefix}" not in merged_symbols or f"__stop_{prefix}" not in merged_symbols:
                raise AssertionError(f"final merge lost {prefix} registry bounds")
        actual = lipo._nm_defined(str(smalltable_merged))
        if "__ejit_shared_start" in actual or "__ejit_shared_end" in actual:
            raise AssertionError("lipo fabricated obsolete shared linker dependencies")
        if "shared_core_state" not in actual:
            raise AssertionError("final merge lost the actual shared data definition")

        minimal = build_archive(root, clang, ar, (), "minimal")
        minimal_gc, _ = gc_merge(lipo, root, minimal, ar, nm, ld, "minimal")
        minimal_symbols = defined_symbols(nm, minimal_gc)
        if "ejit_init" not in minimal_symbols:
            raise AssertionError("mandatory ejit_init root was discarded")
        if (set(DUMP_APIS) | set(SMALL_TABLE_HOOKS) |
                set(SMALL_TABLE_SRE_HOOKS) | set(SMALL_TABLE_DEMO)) & minimal_symbols:
            raise AssertionError("gc-merge fabricated missing optional symbols")

        try:
            gc_merge(lipo, root, minimal, ar, nm, ld, "strict-smalltable",
                     require_small_table_sre=True)
        except SystemExit as error:
            if error.code != 1:
                raise
        else:
            raise AssertionError("--require-small-table-sre accepted missing hooks")

        try:
            gc_merge(lipo, root, minimal, ar, nm, ld, "strict-demo",
                     require_demo=True)
        except SystemExit as error:
            if error.code != 1:
                raise
        else:
            raise AssertionError("--require-demo accepted missing adapters")

        no_callback = build_archive(
            root, clang, ar,
            tuple(symbol for symbol in SMALL_TABLE_DEMO
                  if symbol != "g_pr231_probe_dispatch"),
            "strict-demo-missing-callback",
        )
        try:
            gc_merge(lipo, root, no_callback, ar, nm, ld,
                     "strict-demo-missing-callback", require_demo=True)
        except SystemExit as error:
            if error.code != 1:
                raise
            if "g_pr231_probe_dispatch" not in error.captured_stdout:
                raise AssertionError(
                    "--require-demo did not name the missing callback object: "
                    f"{error.captured_stdout!r}")
        else:
            raise AssertionError("--require-demo accepted a missing callback object")

        duplicate = build_duplicate_root_archive(root, clang, ar, "duplicate")
        try:
            _, duplicate_log = gc_merge(lipo, root, duplicate, ar, nm, ld, "duplicate")
        except SystemExit as error:
            if error.code != 1:
                raise
            if ("duplicate lipo root definitions" not in error.captured_stdout or
                    "ejit_stab_enter" not in error.captured_stdout):
                raise AssertionError(
                    f"duplicate-root error was not diagnosed: "
                    f"{error.captured_stdout!r}")
        else:
            raise AssertionError("gc-merge accepted duplicate strong root definitions")

    print("lipo GC-root regression: PASS")


if __name__ == "__main__":
    main()
