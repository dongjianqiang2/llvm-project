/* merge.ld — Partial-link script for EJIT lipo .a
 *
 * Merges all .o files from the lipo .a into a single relocatable .o,
 * discarding .group (COMDAT) metadata and other link-time-only sections.
 *
 * Usage:
 *   ld.lld -r -o merged.o --script=merge.ld --whole-archive libejit_lipo.a
 */

SECTIONS
{
  /* ── DISCARD ──────────────────────────────────────────────────────── */
  /DISCARD/ : {
    *(.group .group.*)        /*  COMDAT dedup records              */
    *(.rela.debug*)           /*  debug relocations (not needed)     */
    *(.rela.llvm_addrsig*)    /*  address-significance relocs        */
    *(*.llvm_addrsig*)        /*  any leftover addrsig sections      */
    *(.comment .comment.*)    /*  compiler name and version string   */
    *(.note .note.*)          /*  build environment info             */
  }

  /* ── code ─────────────────────────────────────────────────────────── */
  .text           : { *(.text .text.* .gnu.linkonce.t.*) }
  .rodata         : {
    *(.rodata .rodata.* .gnu.linkonce.r.*)
    . = ALIGN(8);
    __start_ejit_bitcode = .;
    KEEP(*(.ejit_bitcode .ejit_bitcode.*))
    __stop_ejit_bitcode = .;

    /* --- EmbeddedJIT period registry --- */
    . = ALIGN(8);
    __start_ejit_period = .;
    KEEP(*(.ejit_period .ejit_period.*))
    __stop_ejit_period = .;
  }
  .data           : { *(.data .data.* .gnu.linkonce.d.*) }
  /* Keep cross-core SRE data in a named section instead of folding it into
   * .data; board link scripts and diagnostics rely on this boundary. */
  .mc_shared      : { KEEP(*(.mc_shared .mc_shared.*)) }
  .bss            : { *(.bss .bss.* .gnu.linkonce.b.*) }

  /* ── init / fini ──────────────────────────────────────────────────── */
  .init           : { *(.init) }
  .fini           : { *(.fini) }
  .init_array : {
      KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*)))
      KEEP (*(.init_array))
  }
  .fini_array     : {
    _fini_array_start = .;
    *(.fini_array .fini_array.*)
    _fini_array_end = .;
  }

  /* ── C++ / unwind ─────────────────────────────────────────────────── */
  .eh_frame       : { *(.eh_frame .eh_frame.*) }
  .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) }
  .tbss           : { *(.tbss .tbss.*) }
  .tdata          : { *(.tdata .tdata.*) }

  /* ── read-write data ──────────────────────────────────────────────── */
  .data.rel.ro    : { *(.data.rel.ro .data.rel.ro.*) }
  .dynamic        : { *(.dynamic) }
  .got            : { *(.got .got.*) }
  .got.plt        : { *(.got.plt) }
  .plt            : { *(.plt .plt.*) }

  /* ── misc known sections ──────────────────────────────────────────── */
  .interp         : { *(.interp) }
  .llvm_addrsig   : { *(.llvm_addrsig) }
  .llvm.call_graph_profile : { *(.llvm.call_graph_profile) }
  .cg_profile      : { *(.cg_profile) }
  .ctors          : { *(.ctors .ctors.*) }
  .dtors          : { *(.dtors .dtors.*) }
  .tm_clone_table : { *(.tm_clone_table) }

  /* ── relocations (keep ― needed by partial link) ──────────────────── */
  .rela.text      : { *(.rela.text .rela.text.*) }
  .rela.rodata    : {
    *(.rela.rodata .rela.rodata.*)
    *(.rela.ejit_bitcode .rela.ejit_bitcode.*)
    *(.rela.ejit_period .rela.ejit_period.*)
  }
  .rela.data      : { *(.rela.data .rela.data.*) }
  .rela.mc_shared : { *(.rela.mc_shared .rela.mc_shared.*) }
  .rela.data.rel.ro : { *(.rela.data.rel.ro .rela.data.rel.ro.*) }
  .rela.eh_frame  : { *(.rela.eh_frame .rela.eh_frame.*) }
  .rela.init_array : { *(.rela.init_array .rela.init_array.*) }
  .rela.plt       : { *(.rela.plt) }

}

/* ld emits .symtab/.strtab for this relocatable object; lipo.py verifies that
 * they remain present after both partial-link stages. They are not discarded
 * by this script. */
