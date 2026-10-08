#!/usr/bin/env python3
"""
EJIT Lipo — Extract the minimal set of .o files from LLVM .a archives.

Three-stage pipeline to produce a single ejit.o from ~36 LLVM .a files:

  extract    Compile reference binary → linker map → nm -u dependency tracing
             → single .a with only the .o files actually needed.
  gc-merge   ld -r --gc-sections on the extracted .a, rooted at EJIT API
             entry points.  Also strips ARM $x/$d mapping symbols and .group
             metadata when llvm-objcopy is available.
  merge      ld -r -T merge.ld → single relocatable ejit.o with merged
             .text/.rodata/.data sections.

Usage:
  python3 lipo.py extract  --arch=x86|aarch64 --build-dir=PATH [--output=PATH]
  python3 lipo.py gc-merge --input=PATH --build-dir=PATH [--output=PATH]
  python3 lipo.py merge    --input=PATH --build-dir=PATH [--output=PATH]

Default compiler/linker are build-dir/bin/clang++ and build-dir/bin/ld.lld.
Override with --cxx / --ld for cross-compilation.

The resulting ejit.o (~30-40 MB) can replace all individual LLVM .a files
when linking EJIT test binaries.

When the EJIT ASM diagnostic dump (ejit_dump_func / ejit_print_dumped /
ejit_print_dumped_module) is enabled on SRE (EJIT_DUMP_ASM=ON under
EJIT_TRIM_LLVM_BACKEND), the assembly emitter formats text into an in-memory
raw_svector_ostream and therefore needs working buffer formatting
(snprintf/vsnprintf). Link a libc that provides them; OR, if the SRE libc lacks
them, add
ejit_test/stubs/ejit_sre_format_stubs.o to the final SRE link/merge command
alongside ejit.o — not both (strong-symbol conflict on snprintf/vsnprintf).
The stub is intentionally NOT merged into ejit.o here so host builds keep
using the platform libc.
"""

import subprocess as sp, os, sys, re, argparse, struct, glob, shutil, tempfile


# ── per-architecture configuration ──────────────────────────────────────────

TARGET_LIBS = {
    "x86": [
        "libLLVMX86CodeGen.a", "libLLVMX86Desc.a", "libLLVMX86Info.a",
    ],
    "aarch64": [
        "libLLVMAArch64CodeGen.a", "libLLVMAArch64Desc.a",
        "libLLVMAArch64Info.a", "libLLVMAArch64Utils.a",
    ],
    "aarch64_be": [
        "libLLVMAArch64CodeGen.a", "libLLVMAArch64Desc.a",
        "libLLVMAArch64Info.a", "libLLVMAArch64Utils.a",
    ],
}

COMMON_LIBS = [
    "libLLVMCore.a", "libLLVMSupport.a", "libLLVMDemangle.a",
    "libLLVMBinaryFormat.a", "libLLVMBitReader.a", "libLLVMBitstreamReader.a",
    "libLLVMAnalysis.a", "libLLVMScalarOpts.a", "libLLVMInstCombine.a",
    "libLLVMipo.a", "libLLVMTransformUtils.a", "libLLVMCodeGen.a",
    "libLLVMCodeGenTypes.a", "libLLVMTarget.a", "libLLVMTargetParser.a",
    "libLLVMSelectionDAG.a", "libLLVMAsmPrinter.a", "libLLVMMC.a",
    "libLLVMObject.a", "libLLVMProfileData.a", "libLLVMInstrumentation.a",
    "libLLVMExecutionEngine.a",
    "libLLVMOrcJIT.a", "libLLVMOrcShared.a", "libLLVMJITLink.a",
    "libLLVMRemarks.a", "libLLVMOption.a", "libLLVMMCDisassembler.a",
    "libLLVMIRPrinter.a",
    "libLLVMOrcTargetProcess.a", "libLLVMRuntimeDyld.a", "libLLVMBitWriter.a",
    "libLLVMGlobalISel.a",
]

# These hooks are referenced by AOT wrappers and the small-table SRE demo only
# after the runtime archive has already been partially linked. Preserve them
# when present, without manufacturing undefined symbols in configurations that
# do not provide the feature.
SMALL_TABLE_API_ROOTS = (
    "ejit_stab_enter",
    "ejit_stab_wrapper_enter",
    "ejit_stab_wrapper_no_policy_current",
    "ejit_stab_leave",
    "ejit_stab_dispatch",
    "ejit_small_table_host_installed",
    "ejit_small_table_published_slots",
)

# SRE-facing C adapters used to bind core-local state and bridge the runtime
# hooks above. These are also optional at the generic lipo stage because older
# runtime archives may not contain the SRE bridge yet.
SMALL_TABLE_SRE_ROOTS = (
    "ejit_small_table_sre_prepare",
    "ejit_small_table_sre_request",
    "ejit_small_table_sre_get_snapshot",
    "ejit_small_table_sre_finish",
    "ejit_small_table_sre_cancel",
    "ejit_small_table_sre_print",
)

# Demo/board entry points are application adapters, not runtime ABI. They are
# rooted only when an adapter object is actually present in the input archive.
SMALL_TABLE_DEMO_ROOTS = (
    "test_ejit_period",
    "test_ejit_smalltable_print",
)

# These public roots are also used by the post-merge verifier. Keep the root
# inventory centralized so the two partial-link stages cannot silently drift.
EJIT_API_ROOTS = (
    "ejit_init", "ejit_init_pgo", "ejit_shutdown", "ejit_activate",
    "ejit_deactivate", "ejit_activate_all", "ejit_deactivate_all",
    "ejit_is_active", "ejit_get_stats", "ejit_register_symbol",
    "ejit_register_bitcode", "ejit_register_period_array",
    "ejit_register_static_var", "ejit_clear_cache", "ejit_compile_or_get",
    "ejit_invalidate", "ejit_set_compile_mode", "ejit_get_compile_mode",
    "ejit_get_last_error", "ejit_set_log_level", "ejit_get_log_level",
    "ejit_print_registry", "ejit_print_func_meta", "ejit_get_code_pool_stats",
    "ejit_get_code_pool_stats_v2", "ejit_print_code_pool_stats",
    "ejit_print_active", "ejit_print_version",
)

OPTIONAL_API_ROOTS = (
    "ejit_register_lifecycle", "ejit_register_funcindex",
    "ejit_taskpool_compile_or_get", "ejit_taskpool_compile_or_get_bound",
    "ejit_taskpool_compile_or_get_bound_v", "ejit_taskpool_release_read",
    "ejit_taskpool_compile_or_get_0d", "ejit_taskpool_compile_or_get_1d",
    "ejit_taskpool_compile_or_get_2d", "ejit_taskpool_compile_or_get_3d",
    "ejit_taskpool_compile_or_get_4d", "ejit_taskpool_set_instance_enabled",
    "ejit_taskpool_pending_count", "ejit_publish_pending_code",
    "ejit_taskpool_get_stats", "ejit_taskpool_print_stats",
    "ejit_taskpool_get_worker_core", "ejit_taskpool_print_compiled",
    "ejit_taskpool_trace_now", "ejit_taskpool_trace_wrapper", "ejit_dump_func",
    "ejit_print_dumped", "ejit_print_dumped_module", "ejit_dump_all",
    "ejit_print_mayconst_ranking", "ejit_register_icache_slot",
    *SMALL_TABLE_API_ROOTS, *SMALL_TABLE_SRE_ROOTS,
    *SMALL_TABLE_DEMO_ROOTS,
)


def all_libs(arch):
    """Return the full list of .a basenames for the given architecture."""
    return COMMON_LIBS + TARGET_LIBS.get(arch, [])


def cxx(build_dir):
    return os.path.join(build_dir, "bin", "clang++")


def ld(build_dir):
    return os.path.join(build_dir, "bin", "ld.lld")


def lib_dir(build_dir):
    return os.path.join(build_dir, "lib")


# ── symbol index ────────────────────────────────────────────────────────────

def build_symbol_index(build_dir):
    """
    Build a mangled-name → (archive_basename, member.o) map from all
    LLVM .a files in the build directory.  Uses nm --print-armap for speed.
    """
    L = lib_dir(build_dir)
    idx = {}
    for a in sorted(glob.glob(os.path.join(L, "libLLVM*.a"))):
        aname = os.path.basename(a)
        r = sp.run(["nm", "--print-armap", a], capture_output=True, text=True)
        for line in r.stdout.split("\n"):
            if " in " in line:
                mangled, member = line.split(" in ", 1)
                idx[mangled.strip()] = (aname, member.strip())
    return idx


# ── objcopy helpers ──────────────────────────────────────────────────────────

def _find_objcopy(build_dir):
    """Return (tool_path, is_llvm) for the best available objcopy.

    Prefer the build's llvm-objcopy (handles extended ELF with >65280
    sections), then llvm-objcopy on PATH, and finally fall back to GNU
    objcopy when no llvm-objcopy is available.
    """
    llvm_oc = os.path.join(build_dir, "bin", "llvm-objcopy")
    if os.path.exists(llvm_oc):
        return llvm_oc, True
    if shutil.which("llvm-objcopy"):
        return "llvm-objcopy", True
    # No llvm-objcopy anywhere; fall back to GNU objcopy.
    return "objcopy", False


def _find_readelf(build_dir):
    """Return an ELF inspection tool; verification fails if none is available."""
    llvm_readelf = os.path.join(build_dir, "bin", "llvm-readelf")
    if os.path.exists(llvm_readelf):
        return llvm_readelf
    return shutil.which("llvm-readelf") or shutil.which("readelf")


def _parse_nm_defined(output):
    """Return symbol -> [(type, value)] from nm -g --defined-only output."""
    symbols = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 2 or len(fields[-2]) != 1:
            continue
        symbol_type, name = fields[-2], fields[-1]
        if symbol_type in ("U", "u", "?"):
            continue
        value = fields[-3] if len(fields) >= 3 else ""
        symbols.setdefault(name, []).append((symbol_type, value))
    return symbols


def _nm_defined(path):
    result = sp.run(["nm", "-g", "--defined-only", path],
                    capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"nm could not inspect defined symbols in {path}: "
            f"{result.stderr[-300:]}")
    return _parse_nm_defined(result.stdout)


def _readelf_sections(path, build_dir):
    return set(_readelf_section_names(path, build_dir))


def _readelf_section_names(path, build_dir):
    tool = _find_readelf(build_dir)
    if not tool:
        raise RuntimeError("no llvm-readelf/readelf available for lipo verification")
    result = sp.run([tool, "-W", "-S", path], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"readelf could not inspect sections in {path}: "
            f"{result.stderr[-300:]}")
    sections = []
    for line in result.stdout.splitlines():
        match = re.match(r"\s*\[\s*\d+\]\s+(\S+)", line)
        if match:
            sections.append(match.group(1))
    if not sections:
        raise RuntimeError(f"readelf found no ELF sections in {path}")
    return sections


def _readelf_is_relocatable(path, build_dir):
    tool = _find_readelf(build_dir)
    if not tool:
        raise RuntimeError("no llvm-readelf/readelf available for lipo verification")
    result = sp.run([tool, "-W", "-h", path], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"readelf could not inspect ELF header in {path}: "
            f"{result.stderr[-300:]}")
    types = re.findall(r"^\s*Type:\s*(.*?)\s*$", result.stdout, re.MULTILINE)
    if not types or any(not re.match(r"REL(?:\s|$)", item) for item in types):
        raise RuntimeError(
            f"lipo output is not entirely relocatable ELF: {path}; types={types}")


def _check_root_definitions(path, build_dir, expected, reject_duplicates=True):
    symbols = _nm_defined(path)
    missing = sorted(set(expected) - set(symbols))
    if missing:
        raise RuntimeError(
            f"lipo output lost required defined symbols: {', '.join(missing)}")
    if reject_duplicates:
        duplicate = sorted(name for name in set(expected)
                           if len(symbols.get(name, ())) > 1)
        if duplicate:
            raise RuntimeError(
                f"lipo output has duplicate root definitions: "
                f"{', '.join(duplicate)}")
    return symbols


def _check_root_duplicates(symbols, roots, path):
    duplicates = sorted(name for name in set(roots)
                        if len(symbols.get(name, ())) > 1)
    if duplicates:
        raise RuntimeError(
            f"input has duplicate lipo root definitions in {path}: "
            f"{', '.join(duplicates)}")


def _check_required_sections(path, build_dir, source_sections,
                             final_merge=False):
    section_names = _readelf_section_names(path, build_dir)
    sections = set(section_names)
    missing = {".symtab", ".strtab"} - sections
    if missing:
        raise RuntimeError(
            f"lipo output is missing ELF symbol/string tables {sorted(missing)}")
    if any(name == ".init_array" or name.startswith(".init_array.")
           for name in source_sections):
        if ".init_array" not in sections and not any(
                name.startswith(".init_array.") for name in sections):
            raise RuntimeError("lipo output discarded input .init_array registrations")
    for name in (".ejit_bitcode", ".ejit_period"):
        present_in_source = any(
            section == name or section.startswith(name + ".")
            for section in source_sections)
        if present_in_source and name not in sections:
            if not final_merge:
                raise RuntimeError(f"lipo GC discarded registration section {name}")
    mc_shared_in_source = any(
        section == ".mc_shared" or section.startswith(".mc_shared.")
        for section in source_sections)
    if mc_shared_in_source:
        if ".mc_shared" not in sections:
            raise RuntimeError("lipo output discarded the explicit .mc_shared section")
        if final_merge:
            if section_names.count(".mc_shared") != 1 or any(
                    section.startswith(".mc_shared.") for section in section_names):
                raise RuntimeError(
                    "final lipo object did not consolidate .mc_shared inputs "
                    "into one explicit .mc_shared output section")
    if final_merge:
        for section, prefix in ((".ejit_bitcode", "ejit_bitcode"),
                                (".ejit_period", "ejit_period")):
            if not any(source == section or source.startswith(section + ".")
                       for source in source_sections):
                continue
            symbols = _nm_defined(path)
            start = symbols.get(f"__start_{prefix}", ())
            stop = symbols.get(f"__stop_{prefix}", ())
            if len(start) != 1 or len(stop) != 1:
                raise RuntimeError(
                    f"final lipo object lost unique {prefix} registration bounds")
            try:
                if int(stop[0][1], 16) <= int(start[0][1], 16):
                    raise RuntimeError(
                        f"final lipo object has empty {prefix} registration range")
            except ValueError as error:
                raise RuntimeError(
                    f"cannot parse {prefix} registration bounds") from error
    return sections


def _try_strip_arm_mapping_symbols(merged_o, work_dir, build_dir):
    """Strip ARM $x/$d mapping symbols from *merged_o* (best-effort).

    $x (code) and $d (data) are ARM ELF mapping symbols inserted by the
    assembler.  They are not needed after a partial link and inflate the
    symtab by ~60 000 entries on aarch64.  Failure is non-fatal: the
    symbols are harmless metadata.
    """
    objcopy_tool, is_llvm = _find_objcopy(build_dir)
    nostrip_o = os.path.join(work_dir, "_nostrip.o")

    r = sp.run([objcopy_tool, "-w", "-N", "$x", "-N", "$d",
                merged_o, nostrip_o], capture_output=True, text=True)
    if r.returncode == 0 and os.path.exists(nostrip_o):
        try:
            before = len(sp.run(["nm", merged_o], capture_output=True,
                                text=True).stdout.splitlines())
            after = len(sp.run(["nm", nostrip_o], capture_output=True,
                               text=True).stdout.splitlines())
            if after < before:
                print(f"       stripped {before - after} $x/$d mapping symbols"
                      f"{' (llvm-objcopy)' if is_llvm else ''}")
                # Replace merged_o with the stripped version
                os.replace(nostrip_o, merged_o)
                return
            os.unlink(nostrip_o)
        except OSError:
            if os.path.exists(nostrip_o):
                os.unlink(nostrip_o)
    elif is_llvm:
        print(f"       note: llvm-objcopy could not strip $x/$d (non-fatal)")
    else:
        print(f"       note: GNU objcopy cannot handle this ELF ("
              f"{'>65280' if True else ''}sections from ld -r)."
              f"  Build llvm-objcopy to enable $x/$d stripping."
              f"  The $x/$d symbols are harmless ARM mapping metadata.")


def _try_remove_group(merged_o, nogroup_o, build_dir):
    """Remove .group (COMDAT) section from *merged_o* (best-effort).

    After ld -r --gc-sections, COMDAT .group metadata is no longer needed.
    merge.ld also discards .group, so failure here is non-fatal.
    """
    objcopy_tool, is_llvm = _find_objcopy(build_dir)
    r = sp.run([objcopy_tool, "--remove-section=.group", merged_o, nogroup_o],
               capture_output=True, text=True)
    if r.returncode == 0 and os.path.exists(nogroup_o):
        before_mb = os.path.getsize(merged_o) / (1024 * 1024)
        after_mb = os.path.getsize(nogroup_o) / (1024 * 1024)
        if after_mb < before_mb:
            print(f"       after --remove-section=.group: {after_mb:.0f} MB")
        else:
            # No size reduction; keep original
            os.unlink(nogroup_o)


# ── extract mode ────────────────────────────────────────────────────────────

def doit_extract(args):
    build_dir = os.path.abspath(args.build_dir)
    arch = args.arch
    L = lib_dir(build_dir)
    # Default output: ejit_test/lipo/ (alongside this script)
    default_out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               f"libejit_lipo_{arch}.a")
    output = args.output or default_out
    work = os.path.join(os.path.dirname(output), f".lipo_work_{arch}")
    os.makedirs(work, exist_ok=True)

    CXX = args.cxx or cxx(build_dir)
    LD = args.ld or ld(build_dir)
    custom_ld = args.ld is not None

    # Quick test object to drive the link
    test_o = os.path.join(work, "_test_main.o")
    sp.run([CXX, "-x", "c", "-", "-O2", "-fno-PIC", "-fno-PIE" , "-c", "-o", test_o],
           input=b"int main(){return 0;}", capture_output=True)

    libs = all_libs(arch)
    all_a = " ".join(os.path.join(L, f) for f in libs
                     if os.path.exists(os.path.join(L, f)))
    ejit_a = os.path.join(L, "libLLVMEJIT.a")

    # ── 1. Build symbol index ────────────────────────────────────────────
    print("[1/4] Building symbol index ...", flush=True)
    sym2file = build_symbol_index(build_dir)
    print(f"       {len(sym2file)} symbols indexed")

    # ── 2. Generate linker map (successful link) ──────────────────────────
    print("[2/4] Generating linker map ...", flush=True)
    if custom_ld:
        ld_dir = os.path.dirname(LD)
        fuse_ld_flag = f"-fuse-ld=lld"
        link_cmd = [CXX, f"-B{ld_dir}", fuse_ld_flag, "-L/tmp"]
    else:
        link_cmd = [CXX, f"-fuse-ld={LD}"]
    r = sp.run(link_cmd + [
        "-Os", "-Wl,--gc-sections",
        "-Wl,--print-map",
        "-Wl,--allow-multiple-definition",
        f"-Wl,--whole-archive", ejit_a, f"-Wl,--no-whole-archive",
        *all_a.split(), "-lpthread", "-Wl,-no-pie", "-ferror-limit=0", "-ldl", test_o,
        "-o", os.path.join(work, "_ref")
    ], capture_output=True, text=True)
    if r.returncode != 0:
        print("ERROR: reference link failed. Is the build directory valid?")
        print(r.stderr[-500:])
        sys.exit(1)
    map_text = r.stdout + r.stderr

    # ── 3. Extract .o from map + trace dependencies ───────────────────────
    print("[3/4] Extracting .o files + tracing dependencies ...", flush=True)
    if args.exclude:
        print(f"       excluding patterns: {args.exclude}")
    pattern = re.compile(r"(libLLVM\S+\.a)\(([^)]+)\)")
    seen = set()   # (aname, member)
    extracted = set()  # unique_name present in work/

    def _is_excluded(member):
        for pat in args.exclude:
            if pat in member:
                return True
        return False

    # Helper: extract member from archive with unique name
    def extract_one(aname, member):
        unique = f"{aname.replace('.a','')}__{member}"
        if unique in extracted:
            return unique
        arch = os.path.join(L, aname)
        sp.run(["ar", "x", arch, member], cwd=work, capture_output=True)
        src = os.path.join(work, member)
        dst = os.path.join(work, unique)
        if os.path.exists(src):
            os.rename(src, dst)
        extracted.add(unique)
        return unique

    # From map
    for m in pattern.finditer(map_text):
        aname, member = m.group(1), m.group(2)
        if _is_excluded(member):
            continue
        key = (aname, member)
        if key not in seen:
            seen.add(key)
            extract_one(aname, member)

    # EJIT (whole-archive)
    sp.run(["ar", "x", ejit_a], cwd=work, capture_output=True)
    for f in os.listdir(work):
        if f.endswith(".o") and not f.startswith("libLLVM"):
            src = os.path.join(work, f)
            dst = os.path.join(work, f"libLLVMEJIT__{f}")
            if os.path.exists(src):
                os.rename(src, dst)
            extracted.add(f"libLLVMEJIT__{f}")

    # Iterative dependency trace via nm -u
    for it in range(10):
        added = 0
        for unique in sorted(extracted):
            o_path = os.path.join(work, unique)
            r = sp.run(["nm", "-u", o_path], capture_output=True, text=True)
            for line in r.stdout.split("\n"):
                parts = line.strip().split()
                if len(parts) >= 2 and parts[0] == "U":
                    mangled = parts[-1]
                    if mangled in sym2file:
                        aname, member = sym2file[mangled]
                        if _is_excluded(member):
                            continue
                        key = (aname, member)
                        if key not in seen:
                            seen.add(key)
                            extract_one(aname, member)
                            added += 1
        if added == 0:
            break
        print(f"       iteration {it+1}: +{added} .o", flush=True)

    # ── 4. Build single .a ────────────────────────────────────────────────
    print(f"[4/4] Building {output} ...", flush=True)
    o_files = [os.path.join(work, f) for f in sorted(os.listdir(work))
               if f.endswith(".o")]
    # Remove old archive to avoid stale members (ar crs only replaces, not deletes)
    if os.path.exists(output):
        os.unlink(output)
    sp.run(["ar", "crs", output, *o_files], capture_output=True)

    sz_mb = os.path.getsize(output) / (1024 * 1024)
    existing = [f for f in libs if os.path.exists(os.path.join(L, f))]
    orig_mb = sum(os.path.getsize(os.path.join(L, f)) for f in existing) / (1024 * 1024)
    print(f"       {len(o_files)} .o files, {sz_mb:.0f} MB")
    print(f"       (from {len(existing)} .a = {orig_mb:.0f} MB)")
    print(f"       output: {output}")


# ── gc-merge mode ───────────────────────────────────────────────────────────

def doit_gc_merge(args):
    input_a = os.path.abspath(args.input)
    output = args.output or os.path.join(os.path.dirname(input_a),
                       os.path.basename(input_a).replace(".a", "_gc.a"))
    work = os.path.join(os.path.dirname(output), ".lipo_gc_work")
    # Clean up any leftover files from previous runs
    if os.path.isdir(work):
        for f in os.listdir(work):
            os.unlink(os.path.join(work, f))
    os.makedirs(work, exist_ok=True)

    LD = args.ld or ld(args.build_dir)
    CXX = cxx(args.build_dir)
    require_small_table = getattr(args, "require_small_table", False)
    require_small_table_sre = getattr(args, "require_small_table_sre", False)
    require_demo = getattr(args, "require_demo", False)
    gc_script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "gc-merge.ld")

    try:
        input_sections = _readelf_sections(input_a, args.build_dir)
        input_symbols = _nm_defined(input_a)
    except RuntimeError as error:
        print(f"ERROR: input archive verification failed: {error}")
        sys.exit(1)

    if require_small_table:
        missing = sorted(set(SMALL_TABLE_API_ROOTS) - set(input_symbols))
        if missing:
            print("ERROR: --require-small-table input is missing runtime hooks: "
                  + ", ".join(missing))
            sys.exit(1)
    if require_small_table_sre:
        required = set(SMALL_TABLE_API_ROOTS) | set(SMALL_TABLE_SRE_ROOTS)
        missing = sorted(required - set(input_symbols))
        if missing:
            print("ERROR: --require-small-table-sre input is missing runtime hooks: "
                  + ", ".join(missing))
            sys.exit(1)
    if require_demo:
        missing = sorted(set(SMALL_TABLE_DEMO_ROOTS) - set(input_symbols))
        if missing:
            print("ERROR: --require-demo input is missing demo adapters: "
                  + ", ".join(missing))
            sys.exit(1)

    # ── 1. Extract .o from input .a ───────────────────────────────────────
    print("[1/3] Extracting .o from input .a ...", flush=True)
    sp.run(["ar", "x", input_a], cwd=work, capture_output=True)
    o_files = [os.path.join(work, f) for f in sorted(os.listdir(work))
               if f.endswith(".o")]
    before_mb = sum(os.path.getsize(o) for o in o_files) / (1024 * 1024)
    print(f"       {len(o_files)} .o files, {before_mb:.0f} MB")

    # ── 2. ld -r --gc-sections ────────────────────────────────────────────
    print("[2/3] Running ld -r --gc-sections ...", flush=True)

    # EJIT API entry points as gc roots. gc-merge runs before the business
    # object is linked, so generated wrapper references are not visible yet.
    # Optional roots are retained only when the input runtime defines them, so
    # taskpool-OFF archives do not acquire unresolved taskpool symbols.
    ejit_api = list(EJIT_API_ROOTS)
    optional_api = list(OPTIONAL_API_ROOTS)

    defined = set(input_symbols)
    retained_optional = [s for s in optional_api if s in defined]
    rooted_definitions = (set(ejit_api) | set(retained_optional)) & defined
    try:
        _check_root_duplicates(input_symbols, rooted_definitions, input_a)
    except RuntimeError as error:
        print(f"ERROR: {error}")
        sys.exit(1)
    ejit_api.extend(retained_optional)
    if retained_optional:
        print("       optional GC roots: " + ", ".join(retained_optional))

    u_flags = []
    for s in ejit_api:
        u_flags.extend(["-u", s])

    merged_o = os.path.join(work, "_merged.o")
    if not os.path.isfile(gc_script):
        print(f"ERROR: GC retention linker script missing: {gc_script}")
        sys.exit(1)
    r = sp.run(
        [LD, "-r", "-o", merged_o, "--gc-sections", "--entry=ejit_init",
         "--allow-multiple-definition", "-T", gc_script]
        + u_flags
        + o_files,
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        print("ERROR: ld -r failed")
        print(r.stderr[:500])
        sys.exit(1)

    after_mb = os.path.getsize(merged_o) / (1024 * 1024)
    print(f"       {before_mb:.0f} MB -> {after_mb:.0f} MB (gc-sections)")

    # ── 2b. Strip ARM $x/$d mapping symbols (metadata, not needed after link) ──
    # $x/$d are ARM mapping symbols (~60K in aarch64) that only help
    # disassemblers; they are safe to strip.  Prefer llvm-objcopy (handles
    # extended ELF with >65280 sections); GNU objcopy rejects such files.
    _try_strip_arm_mapping_symbols(merged_o, work, args.build_dir)

    # ── 2c. Remove .group (COMDAT metadata, not needed after partial link) ──
    # Note: merge.ld also discards .group, so this is a best-effort early clean.
    nogroup_o = os.path.join(work, "_nogroup.o")
    _try_remove_group(merged_o, nogroup_o, args.build_dir)
    if os.path.exists(nogroup_o):
        merged_o = nogroup_o

    try:
        _readelf_is_relocatable(merged_o, args.build_dir)
        _check_required_sections(merged_o, args.build_dir, input_sections)
        _check_root_definitions(merged_o, args.build_dir, rooted_definitions)
    except RuntimeError as error:
        print(f"ERROR: post-GC ELF verification failed: {error}")
        sys.exit(1)

    # ── 3. Build new .a ───────────────────────────────────────────────────
    print(f"[3/3] Building {output} ...", flush=True)
    staged_output = os.path.join(work, "_gc_output.a")
    archive_result = sp.run(["ar", "crs", staged_output, merged_o],
                            capture_output=True, text=True)
    if archive_result.returncode != 0:
        print("ERROR: could not package verified GC object")
        print(archive_result.stderr[-500:])
        sys.exit(1)
    try:
        _readelf_is_relocatable(staged_output, args.build_dir)
        _check_required_sections(staged_output, args.build_dir, input_sections)
        _check_root_definitions(staged_output, args.build_dir,
                                rooted_definitions)
    except RuntimeError as error:
        print(f"ERROR: post-archive ELF verification failed: {error}")
        sys.exit(1)
    os.replace(staged_output, output)
    sz_mb = os.path.getsize(output) / (1024 * 1024)
    print(f"       {sz_mb:.0f} MB")
    print(f"       output: {output}")


# ── merge mode ──────────────────────────────────────────────────────────────

def doit_merge(args):
    """ld -r with merge.ld to produce a single compact .o (ejit.o)."""
    build_dir = os.path.abspath(args.build_dir)
    input_a = os.path.abspath(args.input)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    merge_ld = os.path.join(script_dir, "merge.ld")
    require_small_table = getattr(args, "require_small_table", False)
    require_small_table_sre = getattr(args, "require_small_table_sre", False)
    require_demo = getattr(args, "require_demo", False)

    if not os.path.exists(merge_ld):
        print(f"ERROR: merge.ld not found at {merge_ld}")
        sys.exit(1)

    output = os.path.abspath(args.output or os.path.join(script_dir, "ejit.o"))
    LD = args.ld or ld(build_dir)

    try:
        input_sections = _readelf_sections(input_a, build_dir)
        input_symbols = _nm_defined(input_a)
        _check_required_sections(input_a, build_dir, input_sections)
    except RuntimeError as error:
        print(f"ERROR: input archive verification failed: {error}")
        sys.exit(1)

    if require_small_table:
        missing = sorted(set(SMALL_TABLE_API_ROOTS) - set(input_symbols))
        if missing:
            print("ERROR: --require-small-table input is missing runtime hooks: "
                  + ", ".join(missing))
            sys.exit(1)
    if require_small_table_sre:
        required = set(SMALL_TABLE_API_ROOTS) | set(SMALL_TABLE_SRE_ROOTS)
        missing = sorted(required - set(input_symbols))
        if missing:
            print("ERROR: --require-small-table-sre input is missing runtime hooks: "
                  + ", ".join(missing))
            sys.exit(1)
    if require_demo:
        missing = sorted(set(SMALL_TABLE_DEMO_ROOTS) - set(input_symbols))
        if missing:
            print("ERROR: --require-demo input is missing demo adapters: "
                  + ", ".join(missing))
            sys.exit(1)

    known_roots = set(EJIT_API_ROOTS) | set(OPTIONAL_API_ROOTS)
    rooted_definitions = known_roots & set(input_symbols)
    try:
        _check_root_duplicates(input_symbols, rooted_definitions, input_a)
    except RuntimeError as error:
        print(f"ERROR: {error}")
        sys.exit(1)

    output_dir = os.path.dirname(output)
    if not os.path.isdir(output_dir):
        print(f"ERROR: output directory does not exist: {output_dir}")
        sys.exit(1)
    fd, staged_output = tempfile.mkstemp(prefix=".lipo_merge_", suffix=".o",
                                         dir=output_dir)
    os.close(fd)
    os.unlink(staged_output)

    print(f"[merge] ld -r -T merge.ld -> {output} ...", flush=True)
    try:
        r = sp.run([
            LD, "-r", "-o", staged_output, "-T", merge_ld,
            "--whole-archive", input_a,
        ], capture_output=True, text=True)

        if r.returncode != 0:
            print("ERROR: ld -r failed")
            print(r.stderr[:500])
            sys.exit(1)

        try:
            _readelf_is_relocatable(staged_output, build_dir)
            _check_required_sections(staged_output, build_dir, input_sections,
                                     final_merge=True)
            _check_root_definitions(staged_output, build_dir,
                                    rooted_definitions)
        except RuntimeError as error:
            print(f"ERROR: post-merge ELF verification failed: {error}")
            sys.exit(1)

        os.replace(staged_output, output)
    finally:
        if os.path.exists(staged_output):
            os.unlink(staged_output)

    sz_mb = os.path.getsize(output) / (1024 * 1024)
    print(f"       {sz_mb:.0f} MB")
    print(f"       output: {output}")


# ── main ────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(description="EJIT Lipo tool")
    sub = p.add_subparsers(dest="mode", required=True)

    e = sub.add_parser("extract", help="Extract used .o -> single .a")
    e.add_argument("--arch", required=True, choices=["x86", "aarch64", "aarch64_be"])
    e.add_argument("--build-dir", required=True, help="LLVM build directory")
    e.add_argument("--output", help="Output .a path")
    e.add_argument("--cxx", help="Override C++ compiler (default: build-dir/bin/clang++)")
    e.add_argument("--ld", help="Override linker (default: build-dir/bin/ld.lld)")
    e.add_argument("--exclude", action="append", default=[],
                   help="Exclude .o files matching this substring (repeatable)")

    g = sub.add_parser("gc-merge", help="gc-merge an existing lipo .a")
    g.add_argument("--input", required=True, help="Input .a from extract step")
    g.add_argument("--build-dir", required=True, help="LLVM build directory")
    g.add_argument("--ld", help="Override linker (default: build-dir/bin/ld.lld)")
    g.add_argument("--output", help="Output .a path")
    g.add_argument("--require-small-table", action="store_true",
                   help="Fail unless all seven small-table runtime hooks exist")
    g.add_argument("--require-small-table-sre", action="store_true",
                   help="Fail unless all small-table runtime and SRE bridge hooks exist")
    g.add_argument("--require-demo", action="store_true",
                   help="Fail unless both small-table demo adapters exist")

    m = sub.add_parser("merge", help="ld -r merge into single ejit.o")
    m.add_argument("--input", required=True, help="Input .a from gc-merge step")
    m.add_argument("--build-dir", required=True, help="LLVM build directory")
    m.add_argument("--ld", help="Override linker (default: build-dir/bin/ld.lld)")
    m.add_argument("--output", help="Output .o path (default: ejit.o alongside lipo.py)")
    m.add_argument("--require-small-table", action="store_true",
                   help="Fail unless all seven small-table runtime hooks exist")
    m.add_argument("--require-small-table-sre", action="store_true",
                   help="Fail unless all small-table runtime and SRE bridge hooks exist")
    m.add_argument("--require-demo", action="store_true",
                   help="Fail unless both small-table demo adapters exist")

    args = p.parse_args()

    if args.mode == "extract":
        doit_extract(args)
    elif args.mode == "gc-merge":
        doit_gc_merge(args)
    elif args.mode == "merge":
        doit_merge(args)


if __name__ == "__main__":
    main()
