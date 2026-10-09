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


SHARED_OBJECTS = (
    "g_pr231_config",
    "g_pr231_output",
    "g_pr231_source",
    "g_pr231_stage",
    "g_pr231_probe_arm",
    "g_pr231_probe_result",
    "g_pr231_probe_dispatch",
)
GLOBAL_SHARED_OBJECTS = (
    "g_pr231_config",
    "g_pr231_output",
    "g_pr231_probe_dispatch",
)
SHARED_READ_ONLY = 8
SHARED_READ_WRITE = 9
REGISTRY_ENTRY_BYTES = 40  # Existing ELF64 ejit_reg_entry_t layout, unchanged.


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
    roots = tuple(dict.fromkeys((
        *lipo.SMALL_TABLE_API_ROOTS,
        *lipo.SMALL_TABLE_SRE_ROOTS,
        *lipo.SMALL_TABLE_DEMO_ROOTS,
        "pr231_smalltable_entry",
        "g_pr231_config",
    )))
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
    symbol_tables = {}
    for table_index, section in enumerate(records):
        if section[1] != 2:  # SHT_SYMTAB, not stripped dynsym only.
            continue
        if section[6] >= shnum or section[9] != 24 or section[5] % 24:
            raise RuntimeError("invalid actual ELF symbol table")
        strings = payload(records[section[6]])
        entries = payload(section)
        symbol_tables[table_index] = []
        for off in range(0, len(entries), 24):
            name, info, other, index, value, size = struct.unpack_from(
                endian + "IBBHQQ", entries, off)
            definition = (info >> 4, info & 15, index, value, size)
            symbol_tables[table_index].append(definition)
            if index != 0:
                attributes.setdefault(string(strings, name), []).append(definition)
    for name in roots:
        expected_type = 1 if name in GLOBAL_SHARED_OBJECTS else 2
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
    for name in SHARED_OBJECTS:
        defs = attributes.get(name, ())
        if len(defs) != 1 or defs[0][1] != 1 or defs[0][2] != shared_index:
            raise RuntimeError("fixture data not uniquely in .mc_shared: " + name)
    for name in GLOBAL_SHARED_OBJECTS:
        defs = attributes.get(name, ())
        if len(defs) != 1 or defs[0][0] != 1:
            raise RuntimeError("shared API object is not a unique strong global: " + name)
    # No added __ejit_shared_start/end linker contract. Placement comes from
    # actual section/symbol attributes; the runtime inventory comes from real
    # PASS2 records in the already-kept period registry, not guessed bounds.
    shared_start, shared_end = shared[3], shared[3] + shared[5]
    for name in SHARED_OBJECTS:
        definition = attributes[name][0]
        value, size = definition[3:5]
        if size == 0 or value < shared_start or value >= shared_end or \
                size > shared_end - value:
            raise RuntimeError("shared object extent outside actual section: " + name)
    sections = set(lipo._readelf_section_names(str(obj), str(obj.parent)))
    if not {".symtab", ".strtab", ".init_array", ".mc_shared"} <= sections:
        raise RuntimeError("missing symtab/strtab/init-array/shared section")
    for prefix in ("ejit_bitcode", "ejit_period"):
        start = symbols.get("__start_" + prefix, ())
        stop = symbols.get("__stop_" + prefix, ())
        if len(start) != 1 or len(stop) != 1 or int(stop[0][1], 16) <= int(start[0][1], 16):
            raise RuntimeError("missing or empty actual " + prefix + " registry")
    # Decode pointer relocations, including section-symbol + addend references
    # used by ld -r for local static C objects. Merely finding a registration
    # name or a symbol is NOT proof that its record names the actual object.
    machine = int.from_bytes(data[18:20], order)
    absolute_pointer_reloc = {183: 257, 62: 1}.get(machine)
    if absolute_pointer_reloc is None:
        raise RuntimeError("unsupported target for shared-record pointer verification")
    # Only period-record pointers are consumed below. Do not retain hundreds
    # of thousands of unrelated code/debug relocations in a large runtime ELF.
    period_sections = {definition[2]
        for name in ("__start_ejit_period", "__stop_ejit_period")
        for definition in attributes.get(name, ())}
    relocations = {}
    for section in records:
        if section[1] != 4:  # SHT_RELA.
            continue
        if section[7] not in period_sections:
            continue
        if section[6] not in symbol_tables or section[7] >= shnum or \
                section[9] != 24 or section[5] % 24:
            raise RuntimeError("invalid ELF pointer relocation section")
        relocation_data = payload(section)
        for off in range(0, section[5], 24):
            location, info, addend = struct.unpack_from(
                endian + "QQq", relocation_data, off)
            location -= 0 if elf_type == 1 else records[section[7]][3]
            key = (section[7], location)
            if key in relocations:
                raise RuntimeError("duplicate actual pointer relocation")
            relocations[key] = (section[6], info >> 32,
                                info & 0xffffffff, addend)

    def pointer(section_index, offset):
        relocation = relocations.get((section_index, offset))
        if elf_type == 1:
            if relocation is None:
                raise RuntimeError("missing actual shared-record pointer relocation")
            table, symbol_index, kind, addend = relocation
            if kind != absolute_pointer_reloc or \
                    symbol_index >= len(symbol_tables[table]):
                raise RuntimeError("unsupported actual shared-record pointer relocation")
            symbol = symbol_tables[table][symbol_index]
            target, value = symbol[2], symbol[3] + addend
            if not 0 < target < shnum or value < 0 or value >= records[target][5]:
                raise RuntimeError("unresolved/out-of-range shared-record pointer relocation")
            return target, value
        if elf_type == 3:
            # A loader can rewrite dynamic RELATIVE/ABS64 records. Do not
            # invent their loaded addresses or accept name-only evidence.
            raise RuntimeError("ET_DYN shared-record relocation validation unsupported; "
                               "check the SDK relocatable/final ET_EXEC output")
        if relocation is not None:
            raise RuntimeError("unresolved pointer relocation in linked shared record")
        value = struct.unpack_from(endian + "Q", payload(records[section_index]),
                                   offset)[0]
        targets = [(i, value - s[3]) for i, s in enumerate(records)
                   if s[2] & 2 and s[5] and s[3] <= value < s[3] + s[5]]
        if len(targets) != 1:
            raise RuntimeError("unresolved/ambiguous linked shared-record pointer")
        return targets[0]

    bounds = []
    for name in ("__start_ejit_period", "__stop_ejit_period"):
        defs = attributes.get(name, ())
        if len(defs) != 1 or defs[0][0] != 1 or not 0 < defs[0][2] < shnum:
            raise RuntimeError("invalid actual period registry bound: " + name)
        bounds.append(defs[0])
    begin, end = bounds
    period_index = begin[2]
    start = begin[3] - records[period_index][3]
    stop = end[3] - records[period_index][3]
    if end[2] != period_index or start < 0 or stop > records[period_index][5] or \
            stop <= start or start % 8 or (stop - start) % REGISTRY_ENTRY_BYTES or \
            (stop - start) // REGISTRY_ENTRY_BYTES > 65536:
        raise RuntimeError("invalid actual 40-byte period registry layout")
    period_data = payload(records[period_index])
    inventory = {}
    extents = set()
    shared_record_count = 0
    for offset in range(start, stop, REGISTRY_ENTRY_BYTES):
        tag = struct.unpack_from(endian + "I", period_data, offset)[0]
        if tag not in (SHARED_READ_ONLY, SHARED_READ_WRITE):
            continue
        shared_record_count += 1
        if shared_record_count > 256:
            raise RuntimeError("actual shared-record inventory exceeds runtime capacity")
        if (period_index, offset + 16) in relocations or \
                struct.unpack_from(endian + "Q", period_data, offset + 16)[0]:
            raise RuntimeError("malformed shared-object record schema: name2 must be NULL")
        name_section, name_offset = pointer(period_index, offset + 8)
        name = string(payload(records[name_section]), name_offset)
        target, value = pointer(period_index, offset + 24)
        size = struct.unpack_from(endian + "Q", period_data, offset + 32)[0]
        if target != shared_index or not size or \
                value >= shared[5] or size > shared[5] - value:
            raise RuntimeError("shared record extent outside actual .mc_shared: " + name)
        extent = (value, value + size, tag)
        # Identical TU contributions may coalesce to the same real object.
        # Only exact address/size/access duplicates are safe to coalesce;
        # conflicting access, extent and partial overlaps remain failures.
        extents.add(extent)
        entry = (tag, target, value, size)
        entries = inventory.setdefault(name, [])
        if entry not in entries:
            entries.append(entry)
    for name in SHARED_OBJECTS:
        entries = inventory.get(name, ())
        if len(entries) != 1:
            raise RuntimeError("missing/nonunique actual shared record: " + name)
        definition = attributes[name][0]
        expected = (SHARED_READ_WRITE, shared_index,
                    definition[3] - shared[3], definition[4])
        if entries[0][0] != SHARED_READ_WRITE:
            raise RuntimeError("shared writable fixture has read-only record: " + name)
        if entries[0] != expected:
            raise RuntimeError("shared record does not exactly identify actual object: " + name)
    extents = sorted(extents)
    for left, right in zip(extents, extents[1:]):
        if right[0] < left[1]:
            raise RuntimeError("overlapping actual shared-record extents")
    table = subprocess.check_output(["readelf", "-sW", str(obj)], text=True)
    if not re.search(r"Symbol table '\.symtab'", table):
        raise RuntimeError("readelf cannot read the actual symbol table")
    distinct_required = len(set(roots) | set(SHARED_OBJECTS))
    print(f"[PR231_SYMBOLS] PASS {len(roots)} strong roots + "
          f"{len(SHARED_OBJECTS)} unique shared objects + exact RW registry records; "
          f"{distinct_required} distinct required symbols; ELF64 "
          f"{order}-endian {args.kind}; symbol tables + nonempty registries/init + shared placement")
    if any(name in attributes for name in ("__ejit_shared_start", "__ejit_shared_end")):
        print("[PR231_SYMBOLS] legacy shared bounds present but not required/used")
    print(f"[PR231_SYMBOLS] sha256={hashlib.sha256(data).hexdigest()} file={obj}")
    print("[PR231_SYMBOLS] NOT SDK final-link / DLIB load / board acceptance")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"[PR231_SYMBOLS] FAIL {exc}", file=sys.stderr)
        sys.exit(1)
