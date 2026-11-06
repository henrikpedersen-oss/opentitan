/* Copyright lowRISC contributors.
   Licensed under the Apache License, Version 2.0, see LICENSE for details.
   SPDX-License-Identifier: Apache-2.0

   The assembler suite's tests on Ibex's Simple System: RAM at 0x100000 (1 MiB), Ibex boots at
   0x100080 (the environments put _start at .text.init + 0x80). tohost is SimCtrl's control word,
   as in Simple System's own link.ld: a write with bit 0 set stops the run. */

OUTPUT_ARCH(riscv)
ENTRY(_start)

tohost   = 0x20008;
fromhost = 0x20010;

SECTIONS
{
  . = 0x100000;
  .text.init : { *(.text.init) }
  . = ALIGN(0x100);
  .text : { *(.text) }
  . = ALIGN(0x100);
  .data : {
    PROVIDE(__global_pointer$ = . + 0x800);
    *(.data) *(.sdata)
  }
  .bss : { *(.bss) *(.sbss) }
  _stack_top = 0x200000;
  _end = .;
}
