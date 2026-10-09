/* GC retention for EJIT's linker-discovered registration and shared data.
 *
 * This is an augmenting script (`INSERT` leaves the platform default script
 * in place). The final merge.ld supplies registry bounds; this stage only
 * ensures GC cannot discard the records before those bounds are emitted.
 */
SECTIONS
{
  .ejit_bitcode : { KEEP(*(.ejit_bitcode .ejit_bitcode.*)) }
  .ejit_period  : { KEEP(*(.ejit_period .ejit_period.*)) }
}
INSERT AFTER .rodata;

SECTIONS
{
  /* Keep this named and writable when the input section is writable. */
  .mc_shared : ALIGN(64) {
    __ejit_shared_start = .;
    KEEP(*(.mc_shared .mc_shared.*))
    __ejit_shared_end = .;
  }
}
INSERT AFTER .data;
