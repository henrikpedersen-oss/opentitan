// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal chip reset handoff for an assembler test loaded directly in RRAM. Earl Grey 2 resets
// with MMWP set, so an unmatched M-mode access is denied. The Smepmp rules referenced by Ibex's PMP
// documentation make a locked rule apply to M-mode when MMWP is set. A locked NAPOT region with
// pmpaddr = all ones and R/W/X/A = 1 therefore permits this stand-alone program's chip accesses.

  .section .boot, "ax", @progbits
  // Ibex documentation, Exceptions and Interrupts: reset fetch starts at boot address + 0x80.
  .org 0x80
  .option push
  .option norvc
  .option norelax
  .globl _eg2_reset
  .type _eg2_reset, @function
_eg2_reset:
  li t0, -1
  csrw pmpaddr0, t0
  li t0, 0x9f
  csrw pmpcfg0, t0
  // RRAM controller Programmer's Guide, Enabling Code Execution.
  li t0, 0x41010014
  li t1, 0xa26a38f7
  sw t1, 0(t0)
  fence.i
  la t0, _start
  jr t0
  .size _eg2_reset, . - _eg2_reset
  .option pop
