#!/usr/bin/env python3
"""Read-only final PR231 application symbol/registration check, not board acceptance."""
import argparse
import hashlib
import importlib.util
import pathlib
import re
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("object", type=pathlib.Path)
    parser.add_argument("--require-be", action="store_true")
    parser.add_argument("--kind", choices=("object", "linked"), default="object",
                        help="object: ET_REL; linked: ET_EXEC/ET_DYN, not loader acceptance")
    args = parser.parse_args()
    obj = args.object.resolve(strict=True)
    spec = importlib.util.spec_from_file_location(
        "pr231_lipo", pathlib.Path(__file__).parent / "lipo/lipo.py")
    lipo = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lipo)
    data = obj.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] not in (1, 2):
        raise RuntimeError("not an ELF64 application object")
    if args.require_be and data[5] != 2:
        raise RuntimeError("actual application object is not big endian")
    order = "big" if data[5] == 2 else "little"
    elf_type = int.from_bytes(data[16:18], order)
    if elf_type not in ((1,) if args.kind == "object" else (2, 3)):
        raise RuntimeError("ELF type does not match explicit --kind " + args.kind)
    if args.require_be and int.from_bytes(data[18:20], order) != 183:
        raise RuntimeError("BE object is not AArch64")
    symbols = lipo._nm_defined(str(obj))
    roots = (*lipo.SMALL_TABLE_API_ROOTS, *lipo.SMALL_TABLE_SRE_ROOTS,
             *lipo.SMALL_TABLE_DEMO_ROOTS, "pr231_smalltable_entry",
             "g_pr231_config", "pr231_probe_inflight")
    for name in roots:
        if len(symbols.get(name, ())) != 1:
            raise RuntimeError(f"missing/nonunique actual definition: {name}")
    # Read section and symbol attributes, not just names: merge scripts may
    # emit empty output sections. A weak placeholder is not a real API export.
    endian = ">" if order == "big" else "<"
    header = struct.unpack_from(endian + "HHIQQQIHHHHHH", data, 16)
    shoff, shentsize, shnum, shstrndx = header[5], header[10], header[11], header[12]
    if shentsize != 64 or shnum == 0 or shstrndx >= shnum:
        raise RuntimeError("unsupported or missing ELF section table")
    if shoff + shnum * shentsize > len(data):
        raise RuntimeError("truncated ELF section table")
    records = [struct.unpack_from(endian + "IIQQQQIIQQ", data,
                                  shoff + i * shentsize) for i in range(shnum)]

    def payload(section):
        off, size = section[4:6]
        if section[1] == 8 or off + size > len(data):
            raise RuntimeError("missing/truncated file-backed ELF section")
        return data[off:off + size]

    def string(strings, off):
        if off >= len(strings):
            raise RuntimeError("invalid ELF string offset")
        end = strings.find(b"\0", off)
        if end < 0:
            raise RuntimeError("unterminated ELF string")
        return strings[off:end].decode("utf-8", errors="strict")

    names = payload(records[shstrndx])
    section_by_name = {}
    for i, section in enumerate(records):
        section_by_name.setdefault(string(names, section[0]), []).append(i)
    attributes = {}
    for section in records:
        if section[1] != 2:  # SHT_SYMTAB, not stripped dynsym only.
            continue
        if section[6] >= shnum or section[9] != 24 or section[5] % 24:
            raise RuntimeError("invalid actual ELF symbol table")
        strings = payload(records[section[6]])
        entries = payload(section)
        for off in range(0, len(entries), 24):
            name, info, other, index, value, size = struct.unpack_from(
                endian + "IBBHQQ", entries, off)
            if index != 0:
                attributes.setdefault(string(strings, name), []).append(
                    (info >> 4, info & 15, index))
    for name in roots:
        expected_type = 1 if name == "g_pr231_config" else 2
        if attributes.get(name) is None or len(attributes[name]) != 1 or \
                attributes[name][0][:2] != (1, expected_type):
            raise RuntimeError("not a unique strong typed definition: " + name)
    for name in (".init_array", ".mc_shared"):
        indices = section_by_name.get(name, ())
        if len(indices) != 1 or records[indices[0]][5] == 0:
            raise RuntimeError("missing/nonunique/empty actual section: " + name)
    init = records[section_by_name[".init_array"][0]]
    if init[1] not in (1, 14) or init[5] % 8:
        raise RuntimeError("invalid actual init-array layout")
    shared_index = section_by_name[".mc_shared"][0]
    shared = records[shared_index]
    if shared[2] & 3 != 3:  # SHF_WRITE | SHF_ALLOC; mapping coherence is SDK-owned.
        raise RuntimeError("shared section is not writable/allocated")
    for name in ("g_pr231_config", "g_pr231_output", "g_pr231_source",
                 "g_pr231_stage", "g_pr231_probe_arm", "g_pr231_probe_result"):
        defs = attributes.get(name, ())
        if len(defs) != 1 or defs[0][1] != 1 or defs[0][2] != shared_index:
            raise RuntimeError("fixture data not uniquely in .mc_shared: " + name)
    sections = set(lipo._readelf_section_names(str(obj), str(obj.parent)))
    if not {".symtab", ".strtab", ".init_array", ".mc_shared"} <= sections:
        raise RuntimeError("missing symtab/strtab/init-array/shared section")
    for prefix in ("ejit_bitcode", "ejit_period"):
        start = symbols.get("__start_" + prefix, ())
        stop = symbols.get("__stop_" + prefix, ())
        if len(start) != 1 or len(stop) != 1 or int(stop[0][1], 16) <= int(start[0][1], 16):
            raise RuntimeError("missing or empty actual " + prefix + " registry")
    table = subprocess.check_output(["readelf", "-sW", str(obj)], text=True)
    if not re.search(r"Symbol table '\.symtab'", table):
        raise RuntimeError("readelf cannot read the actual symbol table")
    print(f"[PR231_SYMBOLS] PASS 18 real entry/hook/source definitions; ELF64 "
          f"{order}-endian {args.kind}; symbol tables + nonempty registries/init + shared placement")
    print(f"[PR231_SYMBOLS] sha256={hashlib.sha256(data).hexdigest()} file={obj}")
    print("[PR231_SYMBOLS] NOT SDK final-link / DLIB load / board acceptance")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"[PR231_SYMBOLS] FAIL {exc}", file=sys.stderr)
        sys.exit(1)
