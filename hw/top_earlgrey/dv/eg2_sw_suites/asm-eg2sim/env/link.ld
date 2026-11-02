/* Copyright lowRISC contributors.
   Licensed under the Apache License, Version 2.0, see LICENSE for details.
   SPDX-License-Identifier: Apache-2.0

   The assembler tests on Earl Grey 2. The generated top memory map places the reset ROM at
   0x00040000, RRAM at 0x30000000 and main SRAM at 0x10000000. A small reset handoff in ROM opens
   PMP and jumps to the test at RRAM + 0x80; writable test data and signatures are in SRAM. */

OUTPUT_ARCH(riscv)
ENTRY(_eg2_reset)

MEMORY
{
  rom  (rx)  : ORIGIN = 0x00040000, LENGTH = 0x400
  rram (rx)  : ORIGIN = 0x30000000, LENGTH = 0x200000
  ram  (rw)  : ORIGIN = 0x10000000, LENGTH = 0x20000
}

/* rv_core_ibex.DV_SIM_WINDOW is at cfg base 0x411f0000 + window offset 0x80. */
tohost   = 0x411f0080;
fromhost = 0x411f0084;

SECTIONS
{
  .boot : { KEEP(*(.boot)) } > rom

  .text.init : { *(.text.init) } > rram
  . = ALIGN(0x100);
  .text : { *(.text) *(.text.*) } > rram
  .rodata : { *(.rodata) *(.rodata.*) *(.srodata) *(.srodata.*) } > rram

  .data ORIGIN(ram) : {
    PROVIDE(__global_pointer$ = . + 0x800);
    *(.data) *(.data.*) *(.sdata) *(.sdata.*)
  } > ram
  .bss (NOLOAD) : { *(.bss) *(.bss.*) *(.sbss) *(.sbss.*) *(COMMON) } > ram
  _stack_top = ORIGIN(ram) + LENGTH(ram);
  _end = .;
}

ASSERT(SIZEOF(.boot) <= LENGTH(rom), "EG2 reset handoff is larger than the reset PMP region")
