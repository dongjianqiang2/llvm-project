/*===-- ejit_registry.ld - EmbeddedJIT registry linker script (example) --===*\
|*                                                                            *|
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM          *|
|* Exceptions. See https://llvm.org/LICENSE.txt for license information.      *|
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception                    *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

/*
 * EXAMPLE linker-script fragment for bare-metal targets whose linker does NOT
 * auto-synthesize the __start_/__stop_ bound symbols for custom sections.
 *
 * Background
 * ----------
 * PASS1 (EJitRegisterBitcode) and PASS2 (EJitRegisterPeriod) emit each
 * translation unit's registry entries into INPUT sections named:
 *     .ejit_bitcode   (array of ejit_reg_entry_t: embedded bitcode + symbols)
 *     .ejit_period    (array of ejit_reg_entry_t: period arrays + static vars)
 * Each entry is `struct { i32 type; ptr; ptr; ptr; u64; }` -> 8-byte aligned,
 * 40 bytes on a 64-bit target. Every entry array is private (local) so there
 * is no cross-TU symbol collision; the entries from all TUs must simply end up
 * contiguous so the runtime can iterate them.
 *
 * The runtime (EJit.cpp) walks the half-open range
 *     [__start_ejit_bitcode, __stop_ejit_bitcode)
 *     [__start_ejit_period,  __stop_ejit_period)
 * These four symbols are declared `weak` in the runtime, so if they are left
 * undefined (no ejit_entry functions, or no script) they resolve to 0 and the
 * range is empty. The section names carry a leading dot (.ejit_bitcode), which
 * is the conventional ELF spelling but is NOT a valid C identifier, so the
 * linker never auto-synthesizes __start_/__stop_ for them — this script (or an
 * equivalent in your own linker script) is REQUIRED to define the bounds.
 *
 * What this script does
 * ---------------------
 * It folds the .ejit_bitcode / .ejit_period input sections into a STANDARD
 * output section (here .rodata, since the tables are const) and brackets them
 * with the four bound symbols. KEEP() prevents --gc-sections from dropping the
 * tables (they are only referenced by linker-defined symbols, never by a
 * normal relocation, so they look "unused" to the garbage collector).
 *
 * Integration options
 * -------------------
 *   (A) Paste the two blocks below into your existing .rodata (or .data)
 *       output-section body, between your normal `*(.rodata*)` lines.
 *   (B) Keep them in a separate file and pull it in with INSERT (see the
 *       INSERT variant at the bottom) so you do not have to edit the vendor
 *       script's SECTIONS command.
 *
 * Placement note (.rodata vs .data)
 * ---------------------------------
 * The tables are emitted `constant`; in a static (non-PIC) bare-metal image
 * their pointer fields are resolved to absolute addresses at link time, so
 * they are genuinely read-only and belong in .rodata / flash — no RAM cost.
 * If your layout has no separate .rodata, the identical blocks work inside
 * .data (they will then be copied to RAM by your startup .data init loop).
 */

/* ----------------------------------------------------------------------- */
/* (A) Inline form — drop these two blocks inside an output section body.   */
/*     Shown here inside a complete (skeletal) SECTIONS for clarity.        */
/* ----------------------------------------------------------------------- */

SECTIONS
{
  /* ... your existing .text / .vectors / etc. ... */

  .rodata : ALIGN(8)
  {
    *(.rodata .rodata.*)

    /* --- EmbeddedJIT bitcode registry --- */
    . = ALIGN(8);
    __start_ejit_bitcode = .;
    KEEP(*(.ejit_bitcode))
    __stop_ejit_bitcode = .;

    /* --- EmbeddedJIT period registry --- */
    . = ALIGN(8);
    __start_ejit_period = .;
    KEEP(*(.ejit_period))
    __stop_ejit_period = .;
    . = ALIGN(8);
  } > FLASH      /* or your rodata memory region; use > RAM AT> FLASH for .data */

  /* --- EmbeddedJIT fixed JIT code-pool region (enable: EJIT_FIXED_CODE_POOL) --- */
  /* The runtime reads __ejit_code_start / __ejit_code_end, aligns the start UP to
   * 2MiB, and carves 2MiB-aligned pools from [aligned start, __ejit_code_end)
   * instead of dynamic SRE_MemDbgAlloc - giving a stable JIT address range.
   * The section only needs page (4KiB) alignment: the runtime owns the 2MiB
   * alignment (enable_ex / split_2m_to_4k require 2MiB-aligned bases). The bytes
   * [__ejit_code_start, aligned start) are wasted slack (at most ~2MiB), so size
   * the region with that headroom (16M -> ~14M usable, ~7 pools). If the aligned
   * region is smaller than one pool (2MiB) the runtime falls back to
   * SRE_MemDbgAlloc with a diagnostic. Add (NOLOAD) before the colon to reserve
   * the VMA range without 16M of flash load data (the runtime zero-fills each
   * slab on first use). Under EJIT_FREESTANDING these symbols are strong, so a
   * bare-metal link REQUIRES this block; a missing definition is a hard link
   * error (not silent).
   *
   * Code-segment placement (EJIT_FIXED_CODE_POOL=ON): place in the CODE region,
   * within +-128MiB of .text ("> CODE" below, or after .text in the code
   * segment). The region is RX at load; the runtime enable_rw's each slab
   * (RX->RW) before writing and enable_ex's .text pages (RW->RX) at finalize
   * (.data/.got pages stay RW for GOT writes). Gives direct bl+adrp reach to
   * AOT code. REQUIRES the platform to supply enable_rw (symmetric to
   * enable_ex); a missing enable_rw is a hard link error.
   */
  .text.ejit : ALIGN(4096)
  {
    __ejit_code_start = .;
    . = __ejit_code_start + 16M;
    __ejit_code_end = .;
  } > CODE       /* code segment, near .text (±128MiB bl/adrp reach to AOT) */

  /* ... your existing .bss / etc. ... */
}

/*
 * ----------------------------------------------------------------------- *
 * (B) INSERT form — if you must NOT edit the vendor SECTIONS command.       *
 *     Put ONLY the snippet below in a separate script and pass it after the *
 *     main script. `INSERT AFTER .rodata` (or .data) merges it in without   *
 *     overriding the existing layout. Supported by both GNU ld and ld.lld.  *
 * ----------------------------------------------------------------------- *
 *
 *   SECTIONS
 *   {
 *     .ejit_registry : ALIGN(8)
 *     {
 *       . = ALIGN(8);
 *       __start_ejit_bitcode = .;
 *       KEEP(*(.ejit_bitcode))
 *       __stop_ejit_bitcode = .;
 *
 *       . = ALIGN(8);
 *       __start_ejit_period = .;
 *       KEEP(*(.ejit_period))
 *       __stop_ejit_period = .;
 *       . = ALIGN(8);
 *     }
 *   } INSERT AFTER .rodata;
 *
 * With the INSERT form the linker still places .ejit_registry next to .rodata
 * in the same loadable segment, so it does not become a "custom segment" — it
 * rides in the normal data/rodata program header.
 */
